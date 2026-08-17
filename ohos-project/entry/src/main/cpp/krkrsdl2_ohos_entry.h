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

/* Attach the page XComponent. Registers surface and touch callbacks so the
 * SDL video driver can obtain its native window. */
void OHOS_Entry_AttachXComponent(void *component);

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
