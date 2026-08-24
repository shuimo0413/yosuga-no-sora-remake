/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony event handling for SDL2.
 *
 * Touch events arrive on the ACE UI thread through sdl_ohos_bridge.h and are
 * converted into SDL events. Dispatch to the engine happens through
 * SDL_PushEvent, which is the thread-safe way to queue SDL input from another
 * thread without racing the engine thread's SDL event dispatch
 * and are not safe to call from the UI thread while the engine thread runs).
 */

#include "../../SDL_internal.h"

#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"

#include "SDL_ohosvideo.h"
#include "SDL_ohosevents.h"
#include "sdl_ohos_bridge.h"

#ifndef SDL_TOUCH_MOUSEID
#define SDL_TOUCH_MOUSEID ((SDL_MouseID)-1)
#endif

/* A mutex serialises UI-thread touch dispatch against the engine thread so
 * SDL event state stays consistent. */
static inline SDL_mutex *OHOS_EventMutex(void)
{
	static SDL_mutex *m = NULL;
	if (m == NULL)
	{
		m = SDL_CreateMutex();
	}
	return m;
}

void OHOS_PumpEvents(_THIS)
{
	(void)_this;
	/* Input is pushed directly from the ACE UI thread; nothing to pump. */
}

void SDL_OHOS_OnTouchEvent(int touch_type, float x, float y)
{
	SDL_Window *window = SDL_GetKeyboardFocus();
	if (window == NULL)
	{
		window = SDL_GetMouseFocus();
	}
	if (window == NULL)
	{
		/* Fall back to the first window of the video device. */
		SDL_VideoDevice *device = SDL_GetVideoDevice();
		if (device != NULL && device->windows != NULL)
		{
			window = device->windows;
		}
	}
	/* Touch coords arrive in the physical XComponent pixel space
	 * (vp2px'd in ArkTS). The game lays out in its own logical window space
	 * (1920x1080), so scale from the physical size to the window size. */
	{
		int pw = 0, ph = 0, ww = 0, wh = 0;
		SDL_OHOS_GetPhysicalSize(&pw, &ph);
		if (window) { SDL_GetWindowSize(window, &ww, &wh); }
		if (pw > 0 && ph > 0 && ww > 0 && wh > 0 && (pw != ww || ph != wh))
		{
			x = x * (float)ww / (float)pw;
			y = y * (float)wh / (float)ph;
		}
	}
	int px = (int)(x + 0.5f);
	int py = (int)(y + 0.5f);

	{
		const char *dd = getenv("KRKR_OHOS_DATA_DIR");
		if (dd && dd[0])
		{
			char lpath[512];
			snprintf(lpath, sizeof(lpath), "%s/engine.log", dd);
			FILE *lf = fopen(lpath, "a");
			if (lf) { fprintf(lf, "engine: OnTouchEvent type=%d x=%d y=%d window=%p\n", touch_type, px, py, (void *)window); fclose(lf); }
		}
	}

	if (window == NULL)
	{
		return;
	}
	px = (int)(x + 0.5f);
	py = (int)(y + 0.5f);

	SDL_mutex *mutex = OHOS_EventMutex();
	if (mutex)
	{
		SDL_LockMutex(mutex);
	}

	switch (touch_type)
	{
	case SDL_OHOS_TOUCH_DOWN:
	{
		SDL_Event ev;
		SDL_zero(ev);
		ev.type = SDL_MOUSEMOTION;
		ev.motion.windowID = SDL_GetWindowID(window);
		ev.motion.which = SDL_TOUCH_MOUSEID;
		ev.motion.state = 0;
		ev.motion.x = (Sint16)px;
		ev.motion.y = (Sint16)py;
		ev.motion.xrel = 0;
		ev.motion.yrel = 0;
		SDL_PushEvent(&ev);
		SDL_zero(ev);
		ev.type = SDL_MOUSEBUTTONDOWN;
		ev.button.windowID = SDL_GetWindowID(window);
		ev.button.which = SDL_TOUCH_MOUSEID;
		ev.button.button = SDL_BUTTON_LEFT;
		ev.button.state = SDL_PRESSED;
		ev.button.clicks = 1;
		ev.button.x = px;
		ev.button.y = py;
		SDL_PushEvent(&ev);
		break;
	}
	case SDL_OHOS_TOUCH_MOVE:
	{
		SDL_Event ev;
		SDL_zero(ev);
		ev.type = SDL_MOUSEMOTION;
		ev.motion.windowID = SDL_GetWindowID(window);
		ev.motion.which = SDL_TOUCH_MOUSEID;
		ev.motion.state = 0;
		ev.motion.x = (Sint16)px;
		ev.motion.y = (Sint16)py;
		ev.motion.xrel = 0;
		ev.motion.yrel = 0;
		SDL_PushEvent(&ev);
		break;
	}
	case SDL_OHOS_TOUCH_UP:
	{
		SDL_Event ev;
		SDL_zero(ev);
		ev.type = SDL_MOUSEBUTTONUP;
		ev.button.windowID = SDL_GetWindowID(window);
		ev.button.which = SDL_TOUCH_MOUSEID;
		ev.button.button = SDL_BUTTON_LEFT;
		ev.button.state = SDL_RELEASED;
		ev.button.clicks = 1;
		ev.button.x = px;
		ev.button.y = py;
		SDL_PushEvent(&ev);
		break;
	}
	default:
		break;
	}

	if (mutex)
	{
		SDL_UnlockMutex(mutex);
	}
}

void SDL_OHOS_OnFingerEvent(int finger_id, int touch_type, float x, float y)
{
	SDL_Window *window = SDL_GetKeyboardFocus();
	if (window == NULL)
	{
		window = SDL_GetMouseFocus();
	}
	if (window == NULL)
	{
		SDL_VideoDevice *device = SDL_GetVideoDevice();
		if (device != NULL && device->windows != NULL)
		{
			window = device->windows;
		}
	}
	if (window == NULL)
	{
		return;
	}

	/* SDL finger coordinates are normalised to 0..1 of the window. */
	float nx = x, ny = y;
	{
		int pw = 0, ph = 0;
		SDL_OHOS_GetPhysicalSize(&pw, &ph);
		if (pw > 0 && ph > 0)
		{
			nx = x / (float)pw;
			ny = y / (float)ph;
		}
	}
	if (nx < 0.0f) nx = 0.0f;
	if (nx > 1.0f) nx = 1.0f;
	if (ny < 0.0f) ny = 0.0f;
	if (ny > 1.0f) ny = 1.0f;

	SDL_Event ev;
	SDL_zero(ev);
	switch (touch_type)
	{
	case SDL_OHOS_TOUCH_DOWN:
		ev.type = SDL_FINGERDOWN;
		break;
	case SDL_OHOS_TOUCH_UP:
		ev.type = SDL_FINGERUP;
		break;
	default:
		ev.type = SDL_FINGERMOTION;
		break;
	}
	ev.tfinger.touchId = 0;
	ev.tfinger.fingerId = (SDL_FingerID)(Sint64)finger_id;
	ev.tfinger.x = nx;
	ev.tfinger.y = ny;
	ev.tfinger.dx = 0.0f;
	ev.tfinger.dy = 0.0f;
	ev.tfinger.pressure = 1.0f;
	SDL_PushEvent(&ev);

	/* Trace every DOWN/UP (and a sampling of MOTIONs) into engine.log so the
	 * multi-finger state reaching SDL can be verified on device. */
	{
		static int motion_count = 0;
		int log_it = (touch_type != SDL_OHOS_TOUCH_MOVE) || (++motion_count % 120 == 1);
		if (log_it)
		{
			const char *dd = SDL_OHOS_GetFilesDir();
			if (dd && dd[0])
			{
				char lpath[512];
				snprintf(lpath, sizeof(lpath), "%s/engine.log", dd);
				FILE *lf = fopen(lpath, "a");
				if (lf)
				{
					fprintf(lf, "engine: OnFingerEvent type=%d finger=%d x=%.3f y=%.3f num=%d\n",
						touch_type, finger_id, nx, ny,
						(int)SDL_GetNumTouchFingers(0));
					fclose(lf);
				}
			}
		}
	}
}

void SDL_OHOS_OnSurfaceChanged(int width, int height)
{
	SDL_VideoDevice *device = SDL_GetVideoDevice();
	OHOS_VideoData *videodata;
	SDL_Window *window;

	if (device == NULL || device->driverdata == NULL)
	{
		return;
	}
	videodata = (OHOS_VideoData *)device->driverdata;
	window = videodata->window;
	if (window == NULL || width <= 0 || height <= 0)
	{
		return;
	}

	if (window->w != width || window->h != height)
	{
		/* Update the window fields before pushing the resize event. */
		window->w = width;
		window->h = height;
		SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, width, height);
	}
}
