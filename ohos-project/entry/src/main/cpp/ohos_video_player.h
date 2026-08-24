/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony hardware video player for the KrKriz engine.
 *
 * Uses the Media Kit OH_AVPlayer (which decodes through the AVCodec hardware
 * decoder) and renders straight into the XComponent's OHNativeWindow.
 */
#ifndef OHOS_VIDEO_PLAYER_H
#define OHOS_VIDEO_PLAYER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct NativeWindow;
typedef struct NativeWindow OHNativeWindow;
typedef struct OH_AVPlayer OH_AVPlayer;

namespace Yosuga
{

/* Callback interface used to notify the engine of playback state. */
class VideoPlayerEventListener
{
public:
	virtual ~VideoPlayerEventListener() = default;
	/* Playback reached the end of the file. */
	virtual void OnVideoEnded() = 0;
	/* Playback errored. */
	virtual void OnVideoError(int code) = 0;
};

class OHOSVideoPlayer
{
public:
	OHOSVideoPlayer();
	~OHOSVideoPlayer();

	/* Open and start playback of the given file into the XComponent window. */
	bool Open(const std::string &filePath, OHNativeWindow *nativeWindow, bool loop = false);
	void Stop();
	/* Reclaim all player resources. */
	void Close();
	void Pause();
	void Resume();

	bool IsPlaying() const { return m_playing.load(); }

	void SetListener(VideoPlayerEventListener *listener) { m_listener = listener; }

	/* Called from the AVPlayer async info/error callbacks. The player
	 * pointer identifies which AVPlayer fired them: callbacks from a player
	 * released during a re-open arrive late and must be ignored. */
	void HandleInfo(OH_AVPlayer *player, int type);
	void HandleError(OH_AVPlayer *player, int32_t errorCode);

	/* Global callback fired once when playback reaches EOS. The engine hooks
	 * this to advance the script (SetStatusAsync(Stop)). */
	typedef void (*EndCallback)(void);
	static void SetEndCallback(EndCallback cb);

private:
	/* Release the AVPlayer on a worker thread after COMPLETED (stops the
	 * source-unready callback flood); sets m_player=null under m_mutex. */
	void DelayedRelease();

	OH_AVPlayer *m_player;
	OHNativeWindow *m_nativeWindow;
	std::atomic<bool> m_playing;
	/* Bumped on every Open(): lets delayed workers tell their own player
	 * apart from a newer one opened in the meantime. */
	std::atomic<uint64_t> m_generation{0};
	VideoPlayerEventListener *m_listener;
	std::mutex m_mutex;
};

} // namespace Yosuga

#endif // OHOS_VIDEO_PLAYER_H
