#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TVPMacVideoFinishedCallback)(void *context);

void *TVPMacVideoCreate(const char *path, void *nativeWindow, void *context,
                        TVPMacVideoFinishedCallback finished);
void TVPMacVideoDestroy(void *handle);
void TVPMacVideoPlay(void *handle);
void TVPMacVideoPause(void *handle);
void TVPMacVideoStop(void *handle);
void TVPMacVideoRewind(void *handle);
void TVPMacVideoSetBounds(void *handle, int left, int top, int width, int height);
/* Inform the overlay about the engine's logical window size (game-space, e.g.
   1920x1080) so the platform can map game-space rectangles onto the native
   view with the same aspect-fit transform the renderer uses. No-op where the
   platform does not need it. */
void TVPMacVideoSetScreenGeometry(void *handle, int windowWidth, int windowHeight);
void TVPMacVideoSetVisible(void *handle, int visible);
void TVPMacVideoSetVolume(void *handle, float volume);
void TVPMacVideoSetRate(void *handle, float rate);
void TVPMacVideoSetTime(void *handle, double seconds);
float TVPMacVideoGetVolume(void *handle);
float TVPMacVideoGetRate(void *handle);
int TVPMacVideoHasAudio(void *handle);
int TVPMacVideoGetWidth(void *handle);
int TVPMacVideoGetHeight(void *handle);
double TVPMacVideoGetFPS(void *handle);
double TVPMacVideoGetDuration(void *handle);
double TVPMacVideoGetTime(void *handle);

#ifdef __cplusplus
}
#endif
