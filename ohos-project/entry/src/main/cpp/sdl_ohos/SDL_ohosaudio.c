/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * OpenHarmony OHAudio audio driver for SDL2.
 *
 * OHAudio (OH_AudioRenderer) is callback-driven: the audio service thread
 * asks for PCM through OH_AudioRenderer_OnWriteData. SDL2's mixer runs on
 * its own thread and feeds the device through GetDeviceBuf/PlayDevice, so
 * this driver bridges the two with a ring buffer:
 *
 *   SDL mixer thread: GetDeviceBuf -> SDL_AudioCallback -> PlayDevice
 *                     (PlayDevice copies the mixed period into the ring and
 *                      blocks when the ring is full, throttling the mixer
 *                      to the hardware consumption rate)
 *   OHAudio service : OnWriteData copies whatever is available from the
 *                     ring into the requested buffer; missing data is
 *                     filled with silence (underrun)
 *
 * The renderer is opened in S16LE, matching what FAudio's SDL2 backend
 * requests (krkrz mixes 16-bit PCM); SDL converts other formats if needed.
 */

#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_OHOS

#include "SDL_timer.h"
#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "SDL_ohosaudio.h"

/* The API 12 OHAudio headers use 'bool' without including <stdbool.h>
 * themselves; include it BEFORE them or the SDK 12 sysroot fails to compile. */
#include <stdbool.h>
#include <stdarg.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>
#include <hilog/log.h>

#include "sdl_ohos_bridge.h" /* SDL_OHOS_GetFilesDir for file logging */

/* The ring holds this many SDL mixing periods. Enough headroom for the
 * callback burst pattern of the audio service while keeping latency low. */
#define OHOSAUDIO_RING_PERIODS 8

struct SDL_PrivateAudioData
{
    OH_AudioStreamBuilder *builder;
    OH_AudioRenderer *renderer;
    Uint8 *mixbuf;   /* one SDL mixing period (spec.size bytes) */
    int mixlen;
    Uint8 *ring;     /* ring buffer fed by PlayDevice, drained by OnWriteData */
    int ring_size;
    int ring_read;
    int ring_write;
    SDL_mutex *lock;
    SDL_cond *cond;
    int shutdown;
};

static void OHOSAUDIO_Log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "YosugaOHOS", "audio: %{public}s", line);
    /* hilog is not readable from hdc on this device; mirror into the
     * engine.log files so the audio driver can be diagnosed live. */
    {
        const char *dirs[2];
        int ndirs = 0;
        const char *sandbox = SDL_OHOS_GetFilesDir();
        const char *pub = getenv("KRKR_OHOS_DATA_DIR");
        if (sandbox && sandbox[0]) dirs[ndirs++] = sandbox;
        if (pub && pub[0]) dirs[ndirs++] = pub;
        for (int i = 0; i < ndirs; ++i)
        {
            char lpath[640];
            snprintf(lpath, sizeof(lpath), "%s/engine.log", dirs[i]);
            FILE *lf = fopen(lpath, "a");
            if (lf)
            {
                fprintf(lf, "audio: %s\n", line);
                fclose(lf);
            }
        }
    }
}

static int OHOSAUDIO_RingUsed(struct SDL_PrivateAudioData *hidden)
{
    return (hidden->ring_write - hidden->ring_read + hidden->ring_size) % hidden->ring_size;
}

static int OHOSAUDIO_RingSpace(struct SDL_PrivateAudioData *hidden)
{
    return hidden->ring_size - OHOSAUDIO_RingUsed(hidden);
}

/* --- OHAudio service callbacks (audio service thread) -------------------- */

static int32_t OHOSAUDIO_WriteDataCallback(OH_AudioRenderer *renderer, void *userData,
    void *buffer, int32_t length)
{
    struct SDL_PrivateAudioData *hidden = (struct SDL_PrivateAudioData *)userData;
    (void)renderer;
    if (hidden == NULL || buffer == NULL || length <= 0)
    {
        return AUDIOSTREAM_SUCCESS;
    }
    {
        /* Periodic trace: is the service still requesting PCM? */
        static int cb_count = 0;
        if (++cb_count <= 5 || cb_count % 500 == 1)
        {
            OHOSAUDIO_Log("write callback #%d len=%d", cb_count, (int)length);
        }
    }

    int copied = 0;
    int used;
    int chunk;

    SDL_LockMutex(hidden->lock);
    while (copied < length)
    {
        used = OHOSAUDIO_RingUsed(hidden);
        if (used <= 0)
        {
            break; /* underrun: fill the rest with silence */
        }
        chunk = SDL_min(length - copied, SDL_min(used, hidden->ring_size - hidden->ring_read));
        SDL_memcpy((Uint8 *)buffer + copied, hidden->ring + hidden->ring_read, chunk);
        hidden->ring_read = (hidden->ring_read + chunk) % hidden->ring_size;
        copied += chunk;
    }
    if (copied < length)
    {
        /* S16 silence is 0; fill the missing tail. */
        SDL_memset((Uint8 *)buffer + copied, 0, length - copied);
        {
            static int under_count = 0;
            if (++under_count <= 5 || under_count % 200 == 1)
            {
                OHOSAUDIO_Log("UNDERRUN #%d need=%d got=%d",
                    under_count, (int)length, copied);
            }
        }
    }
    SDL_CondSignal(hidden->cond); /* space was freed */
    SDL_UnlockMutex(hidden->lock);
    return AUDIOSTREAM_SUCCESS;
}

static int32_t OHOSAUDIO_StreamEventCallback(OH_AudioRenderer *renderer, void *userData,
    OH_AudioStream_Event event)
{
    (void)renderer;
    (void)userData;
    OHOSAUDIO_Log("stream event=%d", (int)event);
    return AUDIOSTREAM_SUCCESS;
}

static int32_t OHOSAUDIO_InterruptCallback(OH_AudioRenderer *renderer, void *userData,
    OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint)
{
    (void)renderer;
    (void)userData;
    OHOSAUDIO_Log("interrupt type=%d hint=%d", (int)type, (int)hint);
    return AUDIOSTREAM_SUCCESS;
}

static int32_t OHOSAUDIO_ErrorCallback(OH_AudioRenderer *renderer, void *userData,
    OH_AudioStream_Result error)
{
    (void)renderer;
    (void)userData;
    OHOSAUDIO_Log("error=%d", (int)error);
    return AUDIOSTREAM_SUCCESS;
}

/* --- SDL audio driver interface ------------------------------------------- */

static int OHOSAUDIO_OpenDevice(_THIS, const char *devname)
{
    (void)devname;
    struct SDL_PrivateAudioData *hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*hidden));
    OH_AudioStream_Result res;

    if (hidden == NULL)
    {
        return SDL_OutOfMemory();
    }
    _this->hidden = hidden;

    OH_AudioStreamBuilder *builder = NULL;
    OHOSAUDIO_Log("OpenDevice enter freq=%d ch=%d", _this->spec.freq, _this->spec.channels);
    res = OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
    if (res != AUDIOSTREAM_SUCCESS || builder == NULL)
    {
        OHOSAUDIO_Log("OH_AudioStreamBuilder_Create failed (%d)", (int)res);
        SDL_free(hidden);
        _this->hidden = NULL;
        return SDL_SetError("OHOS: OH_AudioStreamBuilder_Create failed (%d)", (int)res);
    }
    hidden->builder = builder;

    OH_AudioStreamBuilder_SetSamplingRate(builder, _this->spec.freq);
    OH_AudioStreamBuilder_SetChannelCount(builder, _this->spec.channels);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);

    OH_AudioRenderer_Callbacks callbacks;
    callbacks.OH_AudioRenderer_OnWriteData = OHOSAUDIO_WriteDataCallback;
    callbacks.OH_AudioRenderer_OnStreamEvent = OHOSAUDIO_StreamEventCallback;
    callbacks.OH_AudioRenderer_OnInterruptEvent = OHOSAUDIO_InterruptCallback;
    callbacks.OH_AudioRenderer_OnError = OHOSAUDIO_ErrorCallback;
    res = OH_AudioStreamBuilder_SetRendererCallback(builder, callbacks, hidden);
    if (res != AUDIOSTREAM_SUCCESS)
    {
        OHOSAUDIO_Log("SetRendererCallback failed (%d)", (int)res);
    }

    res = OH_AudioStreamBuilder_GenerateRenderer(builder, &hidden->renderer);
    if (res != AUDIOSTREAM_SUCCESS || hidden->renderer == NULL)
    {
        OHOSAUDIO_Log("OH_AudioStreamBuilder_GenerateRenderer failed (%d)", (int)res);
        OH_AudioStreamBuilder_Destroy(builder);
        hidden->builder = NULL;
        SDL_free(hidden);
        _this->hidden = NULL;
        return SDL_SetError("OHOS: OH_AudioStreamBuilder_GenerateRenderer failed (%d)", (int)res);
    }

    /* The device runs in S16LE. Tell SDL so its converter matches. */
    _this->spec.format = AUDIO_S16SYS;
    SDL_CalculateAudioSpec(&_this->spec);

    hidden->mixlen = _this->spec.size;
    hidden->mixbuf = (Uint8 *)SDL_malloc(hidden->mixlen);
    hidden->ring_size = hidden->mixlen * OHOSAUDIO_RING_PERIODS;
    hidden->ring = (Uint8 *)SDL_malloc(hidden->ring_size);
    hidden->lock = SDL_CreateMutex();
    hidden->cond = SDL_CreateCond();
    if (!hidden->mixbuf || !hidden->ring || !hidden->lock || !hidden->cond)
    {
        return SDL_OutOfMemory();
    }
    SDL_memset(hidden->mixbuf, 0, hidden->mixlen);
    SDL_memset(hidden->ring, 0, hidden->ring_size);
    /* Pre-fill 3/4 of the ring with silence: the OHAudio service starts
     * requesting PCM the moment Start() runs, before the SDL mixer thread
     * has produced its first period. Without prefill the ring starts empty
     * and every early callback underruns (silence), which can make the
     * service's first request patterns stall playback. */
    hidden->ring_read = 0;
    hidden->ring_write = (hidden->ring_size * 3) / 4;
    hidden->shutdown = 0;

    OHOSAUDIO_Log("opened %d Hz %d ch %d bytes/period",
        _this->spec.freq, _this->spec.channels, hidden->mixlen);

    res = OH_AudioRenderer_Start(hidden->renderer);
    if (res != AUDIOSTREAM_SUCCESS)
    {
        OHOSAUDIO_Log("OH_AudioRenderer_Start failed (%d)", (int)res);
        return SDL_SetError("OHOS: OH_AudioRenderer_Start failed (%d)", (int)res);
    }
    return 0;
}

static Uint8 *OHOSAUDIO_GetDeviceBuf(_THIS)
{
    struct SDL_PrivateAudioData *hidden = _this->hidden;
    return hidden->mixbuf;
}

static void OHOSAUDIO_PlayDevice(_THIS)
{
    struct SDL_PrivateAudioData *hidden = _this->hidden;
    int size = hidden->mixlen;
    if (size <= 0)
    {
        return;
    }

    SDL_LockMutex(hidden->lock);
    /* Block while the ring cannot take a whole period: the audio service
     * consumes data in its own callback and signals us, which paces the
     * SDL mixer thread to the hardware. */
    {
        static int wait_count = 0;
        int waits = 0;
        while (OHOSAUDIO_RingSpace(hidden) < size && !hidden->shutdown)
        {
            SDL_CondWaitTimeout(hidden->cond, hidden->lock, 200);
            waits++;
        }
        if (waits > 0 && (++wait_count <= 5 || wait_count % 50 == 1))
        {
            OHOSAUDIO_Log("PlayDevice waited %d x 200ms (ring full?)", waits);
        }
    }
    if (!hidden->shutdown)
    {
        int first = SDL_min(size, hidden->ring_size - hidden->ring_write);
        SDL_memcpy(hidden->ring + hidden->ring_write, hidden->mixbuf, first);
        if (size > first)
        {
            SDL_memcpy(hidden->ring, hidden->mixbuf + first, size - first);
        }
        hidden->ring_write = (hidden->ring_write + size) % hidden->ring_size;
    }
    SDL_UnlockMutex(hidden->lock);
}

static void OHOSAUDIO_CloseDevice(_THIS)
{
    struct SDL_PrivateAudioData *hidden = _this->hidden;
    if (hidden == NULL)
    {
        return;
    }
    hidden->shutdown = 1;
    if (hidden->cond)
    {
        SDL_CondSignal(hidden->cond);
    }
    if (hidden->renderer)
    {
        OH_AudioRenderer_Stop(hidden->renderer);
        OH_AudioRenderer_Release(hidden->renderer);
        hidden->renderer = NULL;
    }
    if (hidden->builder)
    {
        OH_AudioStreamBuilder_Destroy(hidden->builder);
        hidden->builder = NULL;
    }
    SDL_free(hidden->mixbuf);
    SDL_free(hidden->ring);
    if (hidden->lock)
    {
        SDL_DestroyMutex(hidden->lock);
    }
    if (hidden->cond)
    {
        SDL_DestroyCond(hidden->cond);
    }
    SDL_free(hidden);
    _this->hidden = NULL;
}

static SDL_bool OHOSAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    /* Set the function pointers */
    impl->OpenDevice = OHOSAUDIO_OpenDevice;
    impl->CloseDevice = OHOSAUDIO_CloseDevice;
    impl->GetDeviceBuf = OHOSAUDIO_GetDeviceBuf;
    impl->PlayDevice = OHOSAUDIO_PlayDevice;

    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->HasCaptureSupport = SDL_FALSE;

    OHOSAUDIO_Log("driver init: OHAudio audio driver registered");
    return SDL_TRUE; /* this audio target is available. */
}

AudioBootStrap OHOSAUDIO_bootstrap = {
    "ohaudio", "OpenHarmony OHAudio driver", OHOSAUDIO_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_OHOS */

/* vi: set ts=4 sw=4 expandtab: */
