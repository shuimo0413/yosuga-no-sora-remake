/* SPDX-License-Identifier: MIT */

#ifndef SDL_ohosgl_h_
#define SDL_ohosgl_h_

#include "../../SDL_internal.h"

#include "SDL_ohosvideo.h"

int OHOS_GL_LoadLibrary(_THIS, const char *path);
void *OHOS_GL_GetProcAddress(_THIS, const char *proc);
void OHOS_GL_UnloadLibrary(_THIS);
SDL_GLContext OHOS_GL_CreateContext(_THIS, SDL_Window *window);
int OHOS_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context);
int OHOS_GL_SetSwapInterval(_THIS, int interval);
int OHOS_GL_GetSwapInterval(_THIS);
int OHOS_GL_SwapWindow(_THIS, SDL_Window *window);
void OHOS_GL_DeleteContext(_THIS, SDL_GLContext context);

#endif /* SDL_ohosgl_h_ */
