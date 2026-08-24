/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony NAPI entry module for the Kirikiri SDL2 engine.
 *
 * The ArkTS shell (EntryAbility/Index) calls these functions through the
 * "entry" NAPI module. The engine then runs on a dedicated thread and renders
 * into the page XComponent surface through the SDL2 OpenHarmony video backend.
 */

#ifndef KRKRSDL2_OHOS_ENTRY_H
#define KRKRSDL2_OHOS_ENTRY_H

#include <napi/native_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Store the UIAbility context and create the rawfile resource manager from
 * it. Must be called from the UI thread before StartEngine. */
void OHOS_Entry_SetResourceManager(napi_env env, napi_value ability_context);

/* Remember the application sandbox files directory (filesDir). */
void OHOS_Entry_SetFilesDir(const char *files_dir);

/* Set the external engine base directory (public Download app folder) and
 * the external savedata directory. The engine prefers them over the sandbox
 * so users can reach the game data and save files. Either may be NULL. */
void OHOS_Entry_SetExternalDirs(const char *base_dir, const char *save_dir);

/* Remember the XComponent surfaceId (API 12+) and create the native
 * window from it, since the legacy surface callbacks do not fire here. */
void OHOS_Entry_SetSurfaceId(const char *surface_id);

/* Remember the VIDEO XComponent surfaceId and create a SEPARATE native
 * window from it. The AVPlayer renders video into this dedicated surface so
 * it never competes with the engine's software framebuffer on the game
 * XComponent. */
void OHOS_Entry_SetVideoSurfaceId(const char *surface_id);

/* Return 1 while the AVPlayer is actively playing video (the ArkTS shell
 * polls this to raise the video XComponent above the game XComponent). */
int OHOS_Entry_IsVideoPlaying(void);

/* 1 while the engine is expected to run: before StartEngine it reports 1
 * (so the shell does not bounce back during startup) and after the engine
 * thread finishes it reports 0, which the shell uses to return to the
 * bootstrap page when the game exits from its in-game menu. */
int OHOS_Entry_IsEngineRunning(void);

/* Set the XComponent surface size in pixels. Called from ArkTS after the
 * component is laid out (the native OnSurfaceChanged callback never fires
 * on this system), so the SDL window size matches the real surface. */
void OHOS_Entry_SetSurfaceSize(uint64_t width, uint64_t height);

/* Attach the page XComponent. Registers surface and touch callbacks so the
 * SDL video driver can obtain its native window. */
void OHOS_Entry_AttachXComponent(void *component);

/* Append native bootstrap diagnostics to <public app dir>/engine.log. */
void OHOS_Entry_LogNative(const char *message);

/* Return the rawfile resource manager created from the ability context, or
 * NULL when initResourceManager has not run yet. */
void *OHOS_Entry_GetResourceManager(void);

/* Start the Kirikiri engine on a dedicated thread. Safe to call once; later
 * calls are ignored. */
void OHOS_Entry_StartEngine(void);

#ifdef __cplusplus
}
#endif

#endif /* KRKRSDL2_OHOS_ENTRY_H */
