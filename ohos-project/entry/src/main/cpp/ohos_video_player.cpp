/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony hardware video player for the KrKriz engine.
 *
 * Uses the Media Kit OH_AVPlayer (hardware decode via AVCodec) and renders
 * directly into the XComponent's OHNativeWindow.
 */

#include "ohos_video_player.h"

#include "sdl_ohos_bridge.h"
#include <multimedia/player_framework/avplayer.h>
#include <multimedia/player_framework/avplayer_base.h>
#include <native_window/external_window.h>

#include <cstdarg>
#include <cstdio>
#include <hilog/log.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <dlfcn.h>

namespace Yosuga
{

static OHOSVideoPlayer *g_cb_self = nullptr;

static void OHOS_VPlayerInfoCallback(OH_AVPlayer *player, AVPlayerOnInfoType type,
	OH_AVFormat */*infoBody*/, void *userData)
{
	OHOSVideoPlayer *self = static_cast<OHOSVideoPlayer *>(userData);
	if (self == nullptr) return;
	self->HandleInfo(player, (int)type);
}

static void OHOS_VPlayerErrorCallback(OH_AVPlayer *player, int32_t errorCode,
	const char */*errorMsg*/, void *userData)
{
	OHOSVideoPlayer *self = static_cast<OHOSVideoPlayer *>(userData);
	if (self == nullptr) return;
	self->HandleError(player, errorCode);
}

static OHOSVideoPlayer::EndCallback g_ohos_end_callback = nullptr;

/* OH_NativeWindow_CleanCache is @since 19; the API 12 CI sysroot headers may
 * not declare it (a plain call then breaks the build). Resolve it at run
 * time through libnative_window.so instead. */
typedef int32_t (*OHNW_CleanCacheFn)(OHNativeWindow *);
static int OHOS_NW_CleanCache(OHNativeWindow *window)
{
	static OHNW_CleanCacheFn fn = nullptr;
	static int resolved = 0;
	if (!resolved)
	{
		resolved = 1;
		void *handle = dlopen("libnative_window.so", RTLD_NOW);
		if (handle != nullptr)
			fn = (OHNW_CleanCacheFn)dlsym(handle, "OH_NativeWindow_CleanCache");
	}
	if (fn == nullptr || window == nullptr)
		return -1;
	return fn(window);
}

void OHOSVideoPlayer::SetEndCallback(EndCallback cb)
{
	g_ohos_end_callback = cb;
}

OHOSVideoPlayer::OHOSVideoPlayer()
	: m_player(nullptr), m_nativeWindow(nullptr), m_playing(false), m_listener(nullptr)
{
}

OHOSVideoPlayer::~OHOSVideoPlayer()
{
	Close();
	g_cb_self = nullptr;
}


bool OHOSVideoPlayer::Open(const std::string &filePath, OHNativeWindow *nativeWindow, bool loop)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_player != nullptr)
	{
		/* Release the previous player SYNCHRONOUSLY. The async Release
		 * leaves the old player's render thread alive on the shared
		 * XComponent window while the new player starts; the two fight over
		 * the surface buffer queue and the video freezes after its first
		 * frame (audio keeps playing). ReleaseSync waits until the old
		 * player is fully torn down and its callbacks have drained. */
		OH_AVPlayer_Stop(m_player);
		OH_AVPlayer_ReleaseSync(m_player);
		m_player = nullptr;
	}
	m_generation.fetch_add(1);
	m_nativeWindow = nativeWindow;
	m_playing = false;

	if (m_nativeWindow == nullptr)
	{
		return false;
	}

	m_player = OH_AVPlayer_Create();
	if (m_player == nullptr)
	{
		return false;
	}

	/* Use an fd source: more reliable than file:// for public-dir paths. */
	int vfd = open(filePath.c_str(), O_RDONLY);
	if (vfd < 0)
	{
		OH_AVPlayer_Release(m_player);
		m_player = nullptr;
		return false;
	}
	struct stat vst;
	if (stat(filePath.c_str(), &vst) != 0 || vst.st_size <= 0)
	{
		close(vfd);
		OH_AVPlayer_Release(m_player);
		m_player = nullptr;
		return false;
	}
	OH_AVErrCode ret = OH_AVPlayer_SetFDSource(m_player, vfd, 0, vst.st_size);
	close(vfd); /* AVPlayer duplicates/keeps its own reference */
	if (ret != AV_ERR_OK)
	{
		OH_AVPlayer_Release(m_player);
		m_player = nullptr;
		return false;
	}

	/* The AVPlayer and the SDL engine share the SAME XComponent native
	 * window. Take the surface over IMMEDIATELY - before Prepare/Play -
	 * so the engine TickBeat (SDL_OHOS_IsVideoPlaying) stops presenting
	 * its framebuffer, and drop any engine buffers still queued on the
	 * surface. Otherwise the engine keeps flushing frames while the
	 * AVPlayer starts, the producers fight over buffer slots and the
	 * video freezes on the first frame (audio keeps going). */
	m_playing = true;
	{
		int32_t cc = OHOS_NW_CleanCache(m_nativeWindow);
	}

	ret = OH_AVPlayer_SetVideoSurface(m_player, m_nativeWindow);
	if (ret != AV_ERR_OK)
	{
		m_playing = false;
		OH_AVPlayer_Release(m_player);
		m_player = nullptr;
		return false;
	}

	/* Keep the video aspect ratio (letterbox) instead of stretching it to
	 * fill the whole window. */
	if (m_nativeWindow)
	{
		OH_NativeWindow_NativeWindowSetScalingMode(m_nativeWindow, 0, OH_SCALING_MODE_SCALE_TO_WINDOW);
	}

	OH_AVPlayer_SetLooping(m_player, loop);

	OH_AVPlayer_SetOnInfoCallback(m_player, OHOS_VPlayerInfoCallback, this);
	OH_AVPlayer_SetOnErrorCallback(m_player, OHOS_VPlayerErrorCallback, this);

	ret = OH_AVPlayer_Prepare(m_player);
	if (ret != AV_ERR_OK)
	{
		m_playing = false;
		OH_AVPlayer_Release(m_player);
		m_player = nullptr;
		return false;
	}
	/* Diagnostic: did the AVPlayer actually open a VIDEO track? If the
	 * width stays 0 the video stream was not attached to the surface and
	 * only the audio plays (exactly the "sound but frozen picture"
	 * symptom). */
	int32_t vw = 0, vh = 0, vdur = 0;
	OH_AVPlayer_GetVideoWidth(m_player, &vw);
	OH_AVPlayer_GetVideoHeight(m_player, &vh);
	OH_AVPlayer_GetDuration(m_player, &vdur);

	/* Set volume after Prepare so audio output is active. */
	OH_AVPlayer_SetVolume(m_player, 1.0f, 1.0f);
	ret = OH_AVPlayer_Play(m_player);
	if (ret != AV_ERR_OK)
	{
		m_playing = false;
		OH_AVPlayer_Release(m_player);
		m_player = nullptr;
		return false;
	}
	m_playing = true;
	return true;
}

/* After playback completes, the engine advances the script (g_ohos_end_callback).
 * The AVPlayer keeps firing OnPosition/OnAmplitude callbacks ("current source is
 * unready") until it is released, which floods the main loop and disturbs the
 * transition. Wait long enough for the game script to finish reading frames,
 * then stop + release on a worker thread (never ReleaseSync on the callback
 * thread - that deadlocks). */
void OHOSVideoPlayer::DelayedRelease()
{
	uint64_t gen = m_generation.load(std::memory_order_relaxed);
	std::thread([this, gen]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		OH_AVPlayer *p = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			/* If the game script opened a NEW video in the meantime, the
			 * generation was bumped: release only the player this call
			 * belongs to, never the new one (that froze the new video). */
			if (m_generation.load(std::memory_order_relaxed) != gen ||
				m_player == nullptr)
			{
				return;
			}
			p = m_player;
			m_player = nullptr;
			/* PLAYBACK COMPLETED: the engine must resume presenting its own
			 * SDL framebuffer once the player is gone. m_playing was set true
			 * in Open() and is what SDL_OHOS_IsVideoPlaying() reads (the engine
			 * TickBeat loop returns early while it is 1, so the menu never
			 * renders and the screen freezes on the last video frame). Clear it
			 * here or the freeze is permanent. */
			m_playing = false;
		}
		if (p)
		{
			OH_AVPlayer_Stop(p);
			OH_AVPlayer_ReleaseSync(p);
		}
	}).detach();
}

void OHOSVideoPlayer::HandleInfo(OH_AVPlayer *player, int type)
{
	if (type == (int)AV_INFO_TYPE_EOS)
	{
		/* Late callbacks from a player released during a re-open must not
		 * clear the new player's m_playing flag (that would let the engine
		 * resume presenting over the new video). */
		if (player != m_player)
		{
			return;
		}
		/* Same as AV_COMPLETED: the engine TickBeat loop skips presenting the
		 * SDL framebuffer while m_playing is 1, so the screen stays on the
		 * video's last frame. Clear it now so the menu can render. */
		m_playing = false;
		if (m_listener) m_listener->OnVideoEnded();
		if (g_ohos_end_callback) g_ohos_end_callback();
	}
	else if (type == (int)AV_INFO_TYPE_STATE_CHANGE)
	{
		/* On this SDK the end-of-stream surfaces as a state change to
		 * AV_COMPLETED rather than an EOS info event. */
		AVPlayerState st = AV_IDLE;
		if (player != nullptr && m_player == player)
		{
			OH_AVPlayer_GetState(player, &st);
		}
		else
		{
			return;
		}
		if (st == AV_PREPARED)
		{
			/* Prepare is asynchronous: this is the first moment the media
			 * info is valid. Log whether the VIDEO track really attached
			 * to the surface (0x0 would mean audio-only playback). */
			int32_t vw = 0, vh = 0, vdur = 0;
			OH_AVPlayer_GetVideoWidth(player, &vw);
			OH_AVPlayer_GetVideoHeight(player, &vh);
			OH_AVPlayer_GetDuration(player, &vdur);
		}
		if (st == AV_COMPLETED)
		{
			/* Clear m_playing IMMEDIATELY, not just in the 1.5s-later
			 * DelayedRelease(). SDL_OHOS_IsVideoPlaying() reads m_playing and
			 * the engine TickBeat loop returns early (skips SDL_RenderPresent)
			 * while it is 1. If we only cleared it in DelayedRelease the flag
			 * stayed 1 through the script transition, so UpdateWindowFramebuffer
			 * never ran and the screen froze on the video's final frame even
			 * though the engine had advanced to the menu. Clearing it here lets
			 * the very next TickBeat present the menu. */
			m_playing = false;
			if (m_listener) m_listener->OnVideoEnded();
			if (g_ohos_end_callback) g_ohos_end_callback();
			/* Release the AVPlayer after the game script has had time to finish
			 * the transition (stops the source-unready callback flood). */
			DelayedRelease();
		}
	}
}

void OHOSVideoPlayer::HandleError(OH_AVPlayer *player, int32_t errorCode)
{
	if (player != m_player)
		return;
	if (m_listener) m_listener->OnVideoError((int)errorCode);
}

void OHOSVideoPlayer::Pause()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_player != nullptr) OH_AVPlayer_Pause(m_player);
	/* KEEP m_playing set: the TJS layer pauses the movie right after
	 * open() to grab the first frame, then play() resumes it. If Pause
	 * cleared m_playing the engine TickBeat would resume presenting its
	 * framebuffer at once and the two producers would fight over the
	 * shared XComponent surface, freezing the video on the first frame.
	 * The surface stays owned by the video (the paused AVPlayer keeps its
	 * last frame on screen); only Stop/Close/COMPLETED hand it back. */
}

void OHOSVideoPlayer::Resume()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_player != nullptr) OH_AVPlayer_Play(m_player);
	m_playing = true;
}

void OHOSVideoPlayer::Stop()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_player != nullptr) OH_AVPlayer_Stop(m_player);
	m_playing = false;
}

void OHOSVideoPlayer::Close()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_player != nullptr)
	{
		OH_AVPlayer_Stop(m_player);
		OH_AVPlayer_ReleaseSync(m_player);
		m_player = nullptr;
	}
	m_playing = false;
	m_nativeWindow = nullptr;
}

} // namespace Yosuga
