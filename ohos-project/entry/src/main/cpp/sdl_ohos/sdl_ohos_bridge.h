/* SPDX-License-Identifier: MIT */
/*
 * Bridge between the OpenHarmony NAPI entry module (libentry.so) and the
 * vendored SDL2 OpenHarmony backend.
 *
 * The declarations in this header are implemented in two places:
 *   - krkrsdl2_ohos_entry.cpp  (libentry.so): files directory, XComponent
 *     surface state and the native window wait/query helpers.
 *   - SDL_ohosevents.c         (libSDL2): touch event delivery.
 *
 * tools/setup_ohos_project.py copies this header into the vendored SDL tree
 * so both sides compile against identical declarations.
 */

#ifndef SDL_OHOS_BRIDGE_H
#define SDL_OHOS_BRIDGE_H

/* Touch types delivered to SDL_OHOS_OnTouchEvent. */
#define SDL_OHOS_TOUCH_DOWN 0
#define SDL_OHOS_TOUCH_UP 1
#define SDL_OHOS_TOUCH_MOVE 2

/* The OpenHarmony NDK builds with -fvisibility=hidden; symbols that cross
 * the libentry.so / libkrkrsdl2.so boundary must be explicitly exported. */
#if defined(__OHOS__) && !defined(OHOS_EXPORT)
#if defined(__GNUC__) || defined(__clang__)
#define OHOS_EXPORT __attribute__((visibility("default")))
#else
#define OHOS_EXPORT
#endif
#elif !defined(OHOS_EXPORT)
#define OHOS_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Store the application sandbox files directory. */
OHOS_EXPORT void SDL_OHOS_SetFilesDir(const char *files_dir);

/* Return the sandbox files directory, or NULL when not set yet. */
OHOS_EXPORT const char *SDL_OHOS_GetFilesDir(void);

/* Store the external game-data directory (may live outside the sandbox,
 * e.g. the public Download folder). SDL_GetBasePath prefers it. */
OHOS_EXPORT void SDL_OHOS_SetDataDir(const char *data_dir);

/* Return the external game-data directory, or NULL when not set. */
OHOS_EXPORT const char *SDL_OHOS_GetDataDir(void);

/* Store the external savedata directory. SDL_GetPrefPath prefers it so the
 * save files stay user-accessible. */
OHOS_EXPORT void SDL_OHOS_SetSaveDir(const char *save_dir);

/* Return the external savedata directory, or NULL when not set. */
OHOS_EXPORT const char *SDL_OHOS_GetSaveDir(void);

/* Block until the XComponent surface provides a native window. Returns 1 when
 * the window is ready, 0 on timeout. */
OHOS_EXPORT int SDL_OHOS_WaitForNativeWindow(int timeout_ms);

/* Return the OHNativeWindow, or NULL when the surface is not ready. */
OHOS_EXPORT void *SDL_OHOS_GetNativeWindow(void);

/* Return the current surface size in pixels. Returns 1 when valid. */
OHOS_EXPORT int SDL_OHOS_GetSurfaceSize(int *width, int *height);

/* Deliver an XComponent touch event. Called from the ACE UI thread. */
OHOS_EXPORT void SDL_OHOS_OnTouchEvent(int touch_type, float x, float y);

/* Notify the SDL video driver that the surface size changed. */
OHOS_EXPORT void SDL_OHOS_OnSurfaceChanged(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* SDL_OHOS_BRIDGE_H */
