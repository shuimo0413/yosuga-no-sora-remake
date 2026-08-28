/* SPDX-License-Identifier: MIT */
/*
 * Video playback bridge between the Kirikiri engine (libkrkrsdl2.so) and the
 * OpenHarmony AVPlayer implementation (libentry.so).
 */
#ifndef OHOS_VIDEO_BRIDGE_H
#define OHOS_VIDEO_BRIDGE_H

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

/* Open a video file and start hardware playback into the XComponent window.
 * Returns 0 on success, -1 on failure. */
OHOS_EXPORT int OHOS_VideoOpen(const char *path, int loop);

/* Stop playback (keep the file/player open). */
OHOS_EXPORT void OHOS_VideoStop(void);

/* Close and release all playback resources. */
OHOS_EXPORT void OHOS_VideoClose(void);

/* Mute/master volume, 0..1. */
OHOS_EXPORT void OHOS_VideoSetVolume(float vol);

/* Register a callback invoked when playback reaches the end. */
typedef void (*OHOS_VideoEndCallback)(void);
OHOS_EXPORT void OHOS_VideoSetEndCallback(OHOS_VideoEndCallback cb);

#ifdef __cplusplus
}
#endif

#endif /* OHOS_VIDEO_BRIDGE_H */
