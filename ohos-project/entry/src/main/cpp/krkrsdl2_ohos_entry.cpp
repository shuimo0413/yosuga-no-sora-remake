/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony NAPI entry implementation.
 *
 * Owns the UIAbility context, the sandbox files directory, the XComponent
 * surface state and the Kirikiri engine thread. The SDL2 OpenHarmony backend
 * reaches the files directory and the native window through the
 * sdl_ohos_bridge.h API implemented here.
 */

#include "krkrsdl2_ohos_entry.h"
#include "ohos_data_extract.h"
#include "sdl_ohos_bridge.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

/* Kirikiri SDL2 platform hooks (see src/core/sdl2/SDLApplication.h).
 * These are plain C++ symbols; keep C++ linkage so the names match the
 * engine's mangled definitions. */
extern void krkrsdl2_pre_init_platform(void);
extern void krkrsdl2_convert_set_args(int argc, char **argv);
extern bool krkrsdl2_init_platform(void);
extern void krkrsdl2_run_main_loop(void);
extern void krkrsdl2_cleanup(void);

#define LOG_DOMAIN 0x0000
#define LOG_TAG "YosugaOHOS"

static void Log(LogLevel level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	OH_LOG_Print(LOG_APP, level, LOG_DOMAIN, LOG_TAG, "%{public}s", buffer);
}

namespace
{

NativeResourceManager *g_resource_manager = nullptr;
std::string g_files_dir;
std::string g_data_dir;
std::string g_save_dir;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
OH_NativeXComponent *g_component = nullptr;
OHNativeWindow *g_native_window = nullptr;
uint64_t g_surface_width = 0;
uint64_t g_surface_height = 0;
bool g_window_ready = false;

bool g_engine_started = false;
std::atomic<bool> g_engine_running{false};

void OnSurfaceCreated(OH_NativeXComponent *component, void *window)
{
	(void)component;
	(void)window;
	Log(LOG_INFO, "XComponent surface created");
}

void OnSurfaceChanged(OH_NativeXComponent *component, void *window)
{
	uint64_t width = 0;
	uint64_t height = 0;
	OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);

	pthread_mutex_lock(&g_lock);
	g_surface_width = width;
	g_surface_height = height;
	// OpenHarmony 5.0: the OnSurfaceChanged window parameter is the
	// OHNativeWindow itself (OH_NativeXComponent_GetNativeWindow removed).
	g_native_window = static_cast<OHNativeWindow *>(window);
	g_window_ready = g_native_window != nullptr;
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);

	SDL_OHOS_OnSurfaceChanged(static_cast<int>(width), static_cast<int>(height));

	Log(LOG_INFO, "XComponent surface changed: %llux%llu window=%p",
		static_cast<unsigned long long>(width),
		static_cast<unsigned long long>(height),
		static_cast<void *>(g_native_window));
}

void OnSurfaceDestroyed(OH_NativeXComponent *component, void *window)
{
	(void)component;
	(void)window;
	pthread_mutex_lock(&g_lock);
	g_native_window = nullptr;
	g_window_ready = false;
	pthread_mutex_unlock(&g_lock);
	Log(LOG_INFO, "XComponent surface destroyed");
}

void OnTouchEvent(OH_NativeXComponent *component, void *window)
{
	OH_NativeXComponent_TouchEvent touch_event;
	OH_NativeXComponent_GetTouchEvent(component, window, &touch_event);

	int touch_type = -1;
	switch (touch_event.type)
	{
	case OH_NATIVEXCOMPONENT_DOWN:
		touch_type = SDL_OHOS_TOUCH_DOWN;
		break;
	case OH_NATIVEXCOMPONENT_UP:
		touch_type = SDL_OHOS_TOUCH_UP;
		break;
	case OH_NATIVEXCOMPONENT_MOVE:
		touch_type = SDL_OHOS_TOUCH_MOVE;
		break;
	case OH_NATIVEXCOMPONENT_CANCEL:
		touch_type = SDL_OHOS_TOUCH_UP;
		break;
	default:
		return;
	}
	SDL_OHOS_OnTouchEvent(touch_type, touch_event.x, touch_event.y);
}

} // namespace

/* ------------------------------------------------------------------------- */
/* sdl_ohos_bridge.h implementation                                          */
/* ------------------------------------------------------------------------- */

void SDL_OHOS_SetFilesDir(const char *files_dir)
{
	if (files_dir != nullptr)
	{
		g_files_dir = files_dir;
	}
}

const char *SDL_OHOS_GetFilesDir(void)
{
	return g_files_dir.empty() ? nullptr : g_files_dir.c_str();
}

void SDL_OHOS_SetDataDir(const char *data_dir)
{
	if (data_dir != nullptr)
	{
		g_data_dir = data_dir;
	}
}

const char *SDL_OHOS_GetDataDir(void)
{
	return g_data_dir.empty() ? nullptr : g_data_dir.c_str();
}

void SDL_OHOS_SetSaveDir(const char *save_dir)
{
	if (save_dir != nullptr)
	{
		g_save_dir = save_dir;
	}
}

const char *SDL_OHOS_GetSaveDir(void)
{
	return g_save_dir.empty() ? nullptr : g_save_dir.c_str();
}

int SDL_OHOS_WaitForNativeWindow(int timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += static_cast<time_t>(timeout_ms / 1000);
	ts.tv_nsec += static_cast<long>((timeout_ms % 1000)) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&g_lock);
	while (!g_window_ready)
	{
		int wait_result = pthread_cond_timedwait(&g_cond, &g_lock, &ts);
		if (wait_result == ETIMEDOUT)
		{
			break;
		}
	}
	int ready = g_window_ready ? 1 : 0;
	pthread_mutex_unlock(&g_lock);
	return ready;
}

void *SDL_OHOS_GetNativeWindow(void)
{
	pthread_mutex_lock(&g_lock);
	void *window = static_cast<void *>(g_native_window);
	pthread_mutex_unlock(&g_lock);
	return window;
}

int SDL_OHOS_GetSurfaceSize(int *width, int *height)
{
	pthread_mutex_lock(&g_lock);
	int valid = g_window_ready ? 1 : 0;
	if (width)
	{
		*width = static_cast<int>(g_surface_width);
	}
	if (height)
	{
		*height = static_cast<int>(g_surface_height);
	}
	pthread_mutex_unlock(&g_lock);
	return valid;
}

/* ------------------------------------------------------------------------- */
/* krkrsdl2_ohos_entry.h implementation                                      */
/* ------------------------------------------------------------------------- */

void OHOS_Entry_SetResourceManager(napi_env env, napi_value ability_context)
{
	if (g_resource_manager != nullptr)
	{
		OH_ResourceManager_ReleaseNativeResourceManager(g_resource_manager);
		g_resource_manager = nullptr;
	}
	g_resource_manager = OH_ResourceManager_InitNativeResourceManager(env, ability_context);
	if (g_resource_manager == nullptr)
	{
		Log(LOG_ERROR, "OH_ResourceManager_InitNativeResourceManager failed");
	}
}

void OHOS_Entry_SetFilesDir(const char *files_dir)
{
	SDL_OHOS_SetFilesDir(files_dir);
	Log(LOG_INFO, "filesDir: %{public}s", files_dir != nullptr ? files_dir : "(null)");
}

void OHOS_Entry_SetExternalDirs(const char *base_dir, const char *save_dir)
{
	SDL_OHOS_SetDataDir(base_dir);
	SDL_OHOS_SetSaveDir(save_dir);
	Log(LOG_INFO, "external baseDir: %{public}s  saveDir: %{public}s",
		base_dir != nullptr ? base_dir : "(null)",
		save_dir != nullptr ? save_dir : "(null)");
}

void *OHOS_Entry_GetResourceManager(void)
{
	return static_cast<void *>(g_resource_manager);
}

void OHOS_Entry_AttachXComponent(void *component)
{
	OH_NativeXComponent *native = static_cast<OH_NativeXComponent *>(component);
	if (native == nullptr)
	{
		Log(LOG_ERROR, "AttachXComponent: null component");
		return;
	}

	pthread_mutex_lock(&g_lock);
	g_component = native;
	pthread_mutex_unlock(&g_lock);

	// Touch events are delivered through the lifecycle callback struct
	// (DispatchTouchEvent); this SDK header does not provide the separate
	// OH_NativeXComponent_TouchEvent_Callback registration API.
	OH_NativeXComponent_Callback callback;
	memset(&callback, 0, sizeof(callback));
	callback.OnSurfaceCreated = OnSurfaceCreated;
	callback.OnSurfaceChanged = OnSurfaceChanged;
	callback.OnSurfaceDestroyed = OnSurfaceDestroyed;
	callback.DispatchTouchEvent = OnTouchEvent;
	OH_NativeXComponent_RegisterCallback(native, &callback);

	Log(LOG_INFO, "XComponent attached");
}

static void EngineMain()
{
	g_engine_running = true;
	Log(LOG_INFO, "engine thread started");

	if (!SDL_OHOS_WaitForNativeWindow(60000))
	{
		Log(LOG_ERROR, "timed out waiting for the XComponent native window");
		g_engine_running = false;
		return;
	}

	// Determine the engine base directory. The ArkTS shell may have set an
	// external (public Download) base with game data at <base>/data and
	// savedata at <save>/. Fall back to the sandbox files directory.
	std::string base_dir = !g_data_dir.empty() ? g_data_dir : g_files_dir;
	bool data_ok = false;
	if (!base_dir.empty())
	{
		// The engine needs a complete data tree (startup.tjs plus the system
		// scripts) or a bundled data.xp3.
		struct stat st;
		std::string startup = base_dir + "/data/startup.tjs";
		std::string system_dir = base_dir + "/data/system";
		std::string xp3 = base_dir + "/data/data.xp3";
		data_ok = ((stat(startup.c_str(), &st) == 0 && S_ISREG(st.st_mode)) &&
		           (stat(system_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))) ||
		          (stat(xp3.c_str(), &st) == 0 && S_ISREG(st.st_mode));
	}
	if (!data_ok)
	{
		// Legacy bundled/mini builds: extract the packaged rawfile data into
		// the sandbox files directory.
		if (g_resource_manager != nullptr && OHOS_ExtractGameData())
		{
			base_dir = g_files_dir;
			data_ok = !base_dir.empty();
		}
	}
	if (!data_ok)
	{
		Log(LOG_ERROR, "game data is missing; use the in-game download or import first");
		g_engine_running = false;
		return;
	}

	if (!base_dir.empty() && chdir(base_dir.c_str()) != 0)
	{
		Log(LOG_WARN, "chdir(%{public}s) failed", base_dir.c_str());
	}

	char app_name[] = "krkrsdl2";
	char *argv[] = {app_name, nullptr};

	try
	{
		krkrsdl2_pre_init_platform();
		krkrsdl2_convert_set_args(1, argv);
		if (krkrsdl2_init_platform())
		{
			// The application asked to terminate during startup.
			g_engine_running = false;
			return;
		}
		krkrsdl2_run_main_loop();
		krkrsdl2_cleanup();
	}
	catch (...)
	{
		Log(LOG_ERROR, "uncaught exception escaped the Kirikiri engine");
	}

	g_engine_running = false;
	Log(LOG_INFO, "engine thread finished");
}

void OHOS_Entry_StartEngine(void)
{
	if (g_engine_started || g_engine_running.load())
	{
		return;
	}
	g_engine_started = true;
	std::thread(EngineMain).detach();
}
