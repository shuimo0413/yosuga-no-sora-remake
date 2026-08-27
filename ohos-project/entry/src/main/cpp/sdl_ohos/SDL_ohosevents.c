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

	/* Window-space coords for the synthetic right-button events below
	 * (same physical->logical scaling as SDL_OHOS_OnTouchEvent). */
	int px = (int)(x + 0.5f);
	int py = (int)(y + 0.5f);
	{
		int pw = 0, ph = 0, ww = 0, wh = 0;
		SDL_OHOS_GetPhysicalSize(&pw, &ph);
		if (window) { SDL_GetWindowSize(window, &ww, &wh); }
		if (pw > 0 && ph > 0 && ww > 0 && wh > 0 && (pw != ww || ph != wh))
		{
			px = (int)(x * (float)ww / (float)pw + 0.5f);
			py = (int)(y * (float)wh / (float)ph + 0.5f);
		}
	}

	/* This backend pushes FINGER events directly without driving SDL's
	 * touch-device state (SDL_SendTouch), so SDL_GetNumTouchFingers stays 0
	 * and the engine's two-finger-tap -> right-mouse-button mapping in
	 * SDLApplication.cpp never fires. Track the active finger count here and
	 * synthesize the right button ourselves: the second finger going down
	 * sends mbRight down, and the count dropping below two sends the up.
	 * Right click is how movies are skipped and menus are backed out of. */
	static int g_ohos_active_fingers = 0;
	if (touch_type == SDL_OHOS_TOUCH_DOWN)
	{
		g_ohos_active_fingers++;
		if (g_ohos_active_fingers == 2)
		{
			SDL_Event evr;
			SDL_zero(evr);
			evr.type = SDL_MOUSEBUTTONDOWN;
			evr.button.windowID = SDL_GetWindowID(window);
			evr.button.which = SDL_TOUCH_MOUSEID;
			evr.button.button = SDL_BUTTON_RIGHT;
			evr.button.state = SDL_PRESSED;
			evr.button.clicks = 1;
			evr.button.x = px;
			evr.button.y = py;
			SDL_PushEvent(&evr);
		}
	}
	else if (touch_type == SDL_OHOS_TOUCH_UP)
	{
		int was_two_or_more = (g_ohos_active_fingers >= 2);
		g_ohos_active_fingers--;
		if (g_ohos_active_fingers < 0) g_ohos_active_fingers = 0;
		if (was_two_or_more && g_ohos_active_fingers <= 1)
		{
			SDL_Event evr;
			SDL_zero(evr);
			evr.type = SDL_MOUSEBUTTONUP;
			evr.button.windowID = SDL_GetWindowID(window);
			evr.button.which = SDL_TOUCH_MOUSEID;
			evr.button.button = SDL_BUTTON_RIGHT;
			evr.button.state = SDL_RELEASED;
			evr.button.clicks = 1;
			evr.button.x = px;
			evr.button.y = py;
			SDL_PushEvent(&evr);
		}
	}

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
}

void SDL_OHOS_OnSurfaceChanged(int width, int height)
{
	/* diagnostic: prove this UI-thread callback does not stall */
	{
		const char *dd = SDL_OHOS_GetFilesDir();
		if (dd && dd[0])
		{
			char lpath[512];
			snprintf(lpath, sizeof(lpath), "%s/diag.txt", dd);
			FILE *lf = fopen(lpath, "a");
			if (lf) { fprintf(lf, "ev: OnSurfaceChanged enter %dx%d\n", width, height); fclose(lf); }
		}
	}
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

void SDL_OHOS_OnMouseEvent(int action, int button, int x, int y)
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
	/* Physical pixels -> logical window space (same as touch). */
	{
		int pw = 0, ph = 0, ww = 0, wh = 0;
		SDL_OHOS_GetPhysicalSize(&pw, &ph);
		SDL_GetWindowSize(window, &ww, &wh);
		if (pw > 0 && ph > 0 && ww > 0 && wh > 0 && (pw != ww || ph != wh))
		{
			x = (int)((float)x * (float)ww / (float)pw + 0.5f);
			y = (int)((float)y * (float)wh / (float)ph + 0.5f);
		}
	}
	SDL_mutex *mutex = OHOS_EventMutex();
	if (mutex)
	{
		SDL_LockMutex(mutex);
	}
	SDL_Event ev;
	SDL_zero(ev);
	if (action == 3)
	{
		/* move: engine cursor tracking and button hover feedback. */
		ev.type = SDL_MOUSEMOTION;
		ev.motion.windowID = SDL_GetWindowID(window);
		ev.motion.which = SDL_TOUCH_MOUSEID;
		ev.motion.x = x;
		ev.motion.y = y;
		ev.motion.xrel = 0;
		ev.motion.yrel = 0;
		ev.motion.state = 0;
	SDL_PushEvent(&ev);
	}
	else if (action == 2)
	{
		/* wheel: x = vertical delta, y = horizontal delta */
		ev.type = SDL_MOUSEWHEEL;
		ev.wheel.windowID = SDL_GetWindowID(window);
		ev.wheel.which = SDL_TOUCH_MOUSEID;
		ev.wheel.x = (Sint32)y;
		ev.wheel.y = (Sint32)x;
		ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
		SDL_PushEvent(&ev);
	}
	else
	{
		ev.type = (action == 0) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		ev.button.windowID = SDL_GetWindowID(window);
		ev.button.which = SDL_TOUCH_MOUSEID;
		ev.button.button = (button == 3) ? SDL_BUTTON_RIGHT :
			(button == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT);
		ev.button.state = (action == 0) ? SDL_PRESSED : SDL_RELEASED;
		ev.button.clicks = 1;
		ev.button.x = x;
		ev.button.y = y;
		SDL_PushEvent(&ev);
	}
	if (mutex)
	{
		SDL_UnlockMutex(mutex);
	}
}

/* OHOS KeyCode (ohos.multimodalInput.keyCode) -> SDL_Scancode for the
 * keys the game uses. Unknown keys return SDL_SCANCODE_UNKNOWN. */
static SDL_Scancode OHOS_KeyToScancode(int code)
{
	if (code >= 2000 && code <= 2009)
	{
		return (SDL_Scancode)(SDL_SCANCODE_0 + (code - 2000));
	}
	if (code >= 2017 && code <= 2042)
	{
		return (SDL_Scancode)(SDL_SCANCODE_A + (code - 2017));
	}
	if (code >= 2090 && code <= 2101)
	{
		return (SDL_Scancode)(SDL_SCANCODE_F1 + (code - 2090));
	}
	switch (code)
	{
	case 2012: return SDL_SCANCODE_UP;
	case 2013: return SDL_SCANCODE_DOWN;
	case 2014: return SDL_SCANCODE_LEFT;
	case 2015: return SDL_SCANCODE_RIGHT;
	case 2054: return SDL_SCANCODE_RETURN;
	case 2070: return SDL_SCANCODE_ESCAPE;
	case 2050: return SDL_SCANCODE_SPACE;
	case 2055: return SDL_SCANCODE_BACKSPACE;
	case 2049: return SDL_SCANCODE_TAB;
	case 2072: return SDL_SCANCODE_LCTRL;
	case 2047: return SDL_SCANCODE_LSHIFT;
	case 2045: return SDL_SCANCODE_LALT;
	case 2043: return SDL_SCANCODE_COMMA;
	case 2044: return SDL_SCANCODE_PERIOD;
	case 2057: return SDL_SCANCODE_MINUS;
	case 2058: return SDL_SCANCODE_EQUALS;
	case 2062: return SDL_SCANCODE_SEMICOLON;
	case 2064: return SDL_SCANCODE_SLASH;
	default: return SDL_SCANCODE_UNKNOWN;
	}
}

void SDL_OHOS_OnKeyEvent(int down, int keycode)
{
	SDL_Scancode sc = OHOS_KeyToScancode(keycode);
	if (sc == SDL_SCANCODE_UNKNOWN)
	{
		return;
	}
	SDL_Window *window = SDL_GetKeyboardFocus();
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
	SDL_mutex *mutex = OHOS_EventMutex();
	if (mutex)
	{
		SDL_LockMutex(mutex);
	}
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.windowID = SDL_GetWindowID(window);
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat = 0;
	ev.key.keysym.scancode = sc;
	ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
	ev.key.keysym.mod = KMOD_NONE;
	SDL_PushEvent(&ev);
	if (mutex)
	{
		SDL_UnlockMutex(mutex);
	}
}
