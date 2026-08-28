/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony video backend for SDL2 (vendored by tools/setup_ohos_project.py).
 * Renders into the page XComponent surface through EGL/GLES.
 */

#ifndef SDL_ohosvideo_h_
#define SDL_ohosvideo_h_

#include "../../SDL_internal.h"

#include "../SDL_sysvideo.h"

#include <EGL/egl.h>

typedef struct SDL_WindowData
{
	SDL_Window *window;
	EGLSurface egl_surface;
	/* software framebuffer support */
	SDL_Surface *framebuffer;
} SDL_WindowData;

typedef struct OHOS_VideoData
{
	SDL_Window *window;
} OHOS_VideoData;

#endif /* SDL_ohosvideo_h_ */
