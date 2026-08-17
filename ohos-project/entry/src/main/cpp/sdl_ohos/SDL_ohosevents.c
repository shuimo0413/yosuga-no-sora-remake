/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony event handling for SDL2.
 *
 * Touch events arrive on the ACE UI thread through sdl_ohos_bridge.h and are
 * converted into SDL mouse events with the touch mouse id, mirroring what the
 * SDL touch-to-mouse hint machinery does on other platforms.
 */

#include "../../SDL_internal.h"

#include "../SDL_sysvideo.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_ohosvideo.h"
#include "SDL_ohosevents.h"
#include "sdl_ohos_bridge.h"

#ifndef SDL_TOUCH_MOUSEID
#define SDL_TOUCH_MOUSEID ((SDL_MouseID)-1)
#endif

void OHOS_PumpEvents(_THIS)
{
	(void)_this;
	/* Input is pushed directly from the ACE UI thread; nothing to pump. */
}

void SDL_OHOS_OnTouchEvent(int touch_type, float x, float y)
{
	SDL_Mouse *mouse = SDL_GetMouse();
	SDL_Window *window = mouse != NULL ? mouse->focus : NULL;
	int px;
	int py;

	if (window == NULL)
	{
		return;
	}
	px = (int)(x + 0.5f);
	py = (int)(y + 0.5f);

	switch (touch_type)
	{
	case SDL_OHOS_TOUCH_DOWN:
		SDL_SendMouseMotion(window, SDL_TOUCH_MOUSEID, 0, px, py);
		SDL_SendMouseButton(window, SDL_TOUCH_MOUSEID, SDL_PRESSED, SDL_BUTTON_LEFT);
		break;
	case SDL_OHOS_TOUCH_MOVE:
		SDL_SendMouseMotion(window, SDL_TOUCH_MOUSEID, 0, px, py);
		break;
	case SDL_OHOS_TOUCH_UP:
		SDL_SendMouseButton(window, SDL_TOUCH_MOUSEID, SDL_RELEASED, SDL_BUTTON_LEFT);
		break;
	default:
		break;
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
		window->w = width;
		window->h = height;
		SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, width, height);
	}
}
