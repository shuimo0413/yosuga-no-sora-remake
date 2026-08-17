/* SPDX-License-Identifier: MIT */

#ifndef SDL_ohosevents_h_
#define SDL_ohosevents_h_

#include "../../SDL_internal.h"

void OHOS_PumpEvents(_THIS);
void SDL_OHOS_OnTouchEvent(int touch_type, float x, float y);
void SDL_OHOS_OnSurfaceChanged(int width, int height);

#endif /* SDL_ohosevents_h_ */
