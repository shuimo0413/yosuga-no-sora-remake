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
#include <multimodalinput/oh_input_manager.h>
#include <arkui/ui_input_event.h>
#include <hilog/log.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/syscall.h>
#include <csignal>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>
#include <dlfcn.h>

/* Catch fatal signals, record a short backtrace into the sandbox
 * files dir (hdc-readable) for diagnosis, and exit cleanly. */
static void OHOS_CrashHandler(int sig, siginfo_t *si, void *uc)
{
	const char *sandbox = SDL_OHOS_GetFilesDir();
	if (sandbox && sandbox[0])
	{
		char path[512];
		snprintf(path, sizeof(path), "%s/crash.txt", sandbox);
		FILE *lf = fopen(path, "a");
		if (lf)
		{
			fprintf(lf, "===== CRASH signal=%d (%s) tid=%lu =====\n",
				sig, strsignal(sig), (unsigned long)syscall(__NR_gettid));
			if (si) fprintf(lf, "  fault addr = %p\n", si->si_addr);
#if defined(__aarch64__)
			if (uc)
			{
				const unsigned char *b = (const unsigned char *)uc;
				unsigned long pc = 0, sp = 0, lr = 0;
				memcpy(&pc, b + 440, 8);
				memcpy(&sp, b + 432, 8);
				memcpy(&lr, b + 424, 8); /* regs[30] = LR */
				fprintf(lf, "  pc=%p sp=%p lr(x30)=%p\n",
					(void *)pc, (void *)sp, (void *)lr);
				Dl_info info;
				memset(&info, 0, sizeof(info));
				if (pc && dladdr((void *)pc, &info) && info.dli_fname)
					fprintf(lf, "  pc in: %s +0x%lx\n", info.dli_fname,
						(unsigned long)(pc - (unsigned long)info.dli_fbase));
				if (lr && dladdr((void *)lr, &info) && info.dli_fname)
					fprintf(lf, "  lr in: %s +0x%lx\n", info.dli_fname,
						(unsigned long)(lr - (unsigned long)info.dli_fbase));
			}
#endif
			fflush(lf);
			fclose(lf);
		}
	}
	_exit(128 + sig);
}

void OHOS_InstallCrashHandler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = OHOS_CrashHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
}


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

/* Diagnostic trace into the sandbox diag.txt (hdc-readable). */
static void OHOS_DiagLog(const char *fmt, ...)
{
	const char *sandbox = SDL_OHOS_GetFilesDir();
	if (!sandbox || !sandbox[0])
		return;
	char path[512];
	snprintf(path, sizeof(path), "%s/diag.txt", sandbox);
	FILE *lf = fopen(path, "a");
	if (!lf)
		return;
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	fprintf(lf, "%lld: %s\n", ms % 100000, line);
	fclose(lf);
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
/* Separate native window for the AVPlayer video: created from the video
 * XComponent's surfaceId. The engine framebuffer and the video never share
 * a buffer queue. */
OHNativeWindow *g_video_native_window = nullptr;
uint64_t g_surface_width = 0;
uint64_t g_surface_height = 0;
/* Physical XComponent pixel size reported from ArkTS; used only to scale
 * touch coordinates into the game's logical window space. The SDL window
 * size itself stays at the game resolution (1920x1080). */
uint64_t g_physical_width = 0;
uint64_t g_physical_height = 0;
bool g_window_ready = false;

bool g_engine_started = false;
std::atomic<bool> g_engine_running{false};

/* Mouse-wheel / axis events (API 12): XComponent UI input callback gives
 * the raw scroll deltas that neither ArkUI MouseEvent nor
 * OH_NativeXComponent_MouseEvent expose. Forward to the SDL backend as
 * SDL_MOUSEWHEEL (action 2: x = vertical delta, y = horizontal). */
static void OnUIInputEvent(OH_NativeXComponent *component, ArkUI_UIInputEvent *event,
	ArkUI_UIInputEvent_Type type)
{
	(void)component;
	if (type != ARKUI_UIINPUTEVENT_TYPE_AXIS || event == nullptr) return;
	int yv = (int)OH_ArkUI_AxisEvent_GetVerticalAxisValue(event);
	int xv = (int)OH_ArkUI_AxisEvent_GetHorizontalAxisValue(event);
	if (yv != 0 || xv != 0)
	{
		SDL_OHOS_OnMouseEvent(2, 0, yv, xv);
	}
	}

/* System mouse event monitor (API 12+, multimodalinput): the phone treats
 * the physical mouse as a touch source, so ArkUI never delivers real
 * right/middle clicks or wheel deltas. This global monitor gets them at
 * the input layer: right/middle buttons -> SDL mouse (left stays on the
 * touch path), vertical axis -> SDL_MOUSEWHEEL (positive = up). */
static bool g_mouse_monitor_ok = false;
static void OnSystemMouseEvent(const Input_MouseEvent *ev)
{
	if (ev == nullptr) return;
	int32_t action = OH_Input_GetMouseEventAction(ev);
	int32_t button = OH_Input_GetMouseEventButton(ev);
	int32_t axisType = OH_Input_GetMouseEventAxisType(ev);
  float axisValue = 0.0f;
	/* HarmonyOS NEXT exposes OH_Input_GetMouseEventWheelDelta while the
	 * OpenHarmony SDK uses OH_Input_GetMouseEventAxisValue; resolve at run
	 * time so either system reports the wheel delta. */
	{
		typedef int16_t (*WheelDeltaFn)(const struct Input_MouseEvent*);
		static WheelDeltaFn wd = nullptr;
		if (wd == nullptr)
		{
			void *h = dlopen("libentry.so", RTLD_NOW);
			if (!h) h = RTLD_DEFAULT;
			wd = (WheelDeltaFn)dlsym(h, "OH_Input_GetMouseEventWheelDelta");
		}
		if (wd) axisValue = (float)wd(ev);
		if (axisValue == 0.0f) axisValue = OH_Input_GetMouseEventAxisValue(ev);
	}
	int32_t x = OH_Input_GetMouseEventDisplayX(ev);
	int32_t y = OH_Input_GetMouseEventDisplayY(ev);
	switch (action)
	{
	case MOUSE_ACTION_BUTTON_DOWN:
	case MOUSE_ACTION_BUTTON_UP:
	{
		if (button == MOUSE_BUTTON_RIGHT || button == MOUSE_BUTTON_MIDDLE)
		{
			int b = (button == MOUSE_BUTTON_RIGHT) ? 3 : 2;
			int a = (action == MOUSE_ACTION_BUTTON_DOWN) ? 0 : 1;
			SDL_OHOS_OnMouseEvent(a, b, x, y);
		}
		break;
	}
	case MOUSE_ACTION_AXIS_BEGIN:
	case MOUSE_ACTION_AXIS_UPDATE:
	{
		if (axisType == MOUSE_AXIS_SCROLL_VERTICAL && axisValue != 0.0f)
		{
			SDL_OHOS_OnMouseEvent(2, 0, (int)axisValue, 0);
		}
		break;
	}
	default: break;
	}
}

static void RegisterSystemMouseMonitor()
{
	if (g_mouse_monitor_ok) return;
	Input_Result rc = OH_Input_AddMouseEventMonitor(OnSystemMouseEvent);
	g_mouse_monitor_ok = (rc == INPUT_SUCCESS);
	OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003333, "SDL_OHOS",
		"mouse monitor: %{public}d", (int)rc);
}

void OnSurfaceCreated(OH_NativeXComponent *component, void *window)
{
	(void)component;
	(void)window;
	RegisterSystemMouseMonitor();
}

void OnSurfaceChanged(OH_NativeXComponent *component, void *window)
{
	// A window resize races the surface teardown on KaihongOS: the
	// framework can deliver a null window here, and
	// OH_NativeXComponent_GetXComponentSize then dereferences it
	// (SIGSEGV inside libace XComponentPattern::OnSurfaceChanged).
	if (component == nullptr || window == nullptr)
	{
		OHOS_DiagLog("OnSurfaceChanged(ui) null component/window; ignoring");
		return;
	}
	uint64_t width = 0;
	uint64_t height = 0;
	int32_t sz = OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);

	pthread_mutex_lock(&g_lock);
	g_surface_width = width;
	g_surface_height = height;
	// OpenHarmony 5.0: the OnSurfaceChanged window parameter is the
	// OHNativeWindow itself (OH_NativeXComponent_GetNativeWindow removed).
	g_native_window = static_cast<OHNativeWindow *>(window);
	g_window_ready = g_native_window != nullptr;
	pthread_cond_broadcast(&g_cond);
	pthread_mutex_unlock(&g_lock);

	if (g_window_ready)
	{
		OHOS_DiagLog("OnSurfaceChanged(ui) before SDL_OHOS_OnSurfaceChanged");
		SDL_OHOS_OnSurfaceChanged(static_cast<int>(width), static_cast<int>(height));
		OHOS_DiagLog("OnSurfaceChanged(ui) after SDL_OHOS_OnSurfaceChanged");
	}
}

void OnSurfaceDestroyed(OH_NativeXComponent *component, void *window)
{
	(void)component;
	(void)window;
	pthread_mutex_lock(&g_lock);
	g_native_window = nullptr;
	g_window_ready = false;
	pthread_mutex_unlock(&g_lock);
}

[[maybe_unused]] void OnTouchEvent(OH_NativeXComponent *component, void *window)
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
}

void OHOS_Entry_SetFilesDir(const char *files_dir)
{
	SDL_OHOS_SetFilesDir(files_dir);
}
void OHOS_Entry_SetSurfaceId(const char *surface_id)
{
	if (surface_id == nullptr || surface_id[0] == '\0')
	{
		return;
	}
	uint64_t id = 0;
	try
	{
		id = static_cast<uint64_t>(std::strtoull(surface_id, nullptr, 10));
	}
	catch (...)
	{
		return;
	}

	OHNativeWindow *window = nullptr;
	int32_t ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(id, &window);
	if (ret == 0 && window != nullptr)
	{
		pthread_mutex_lock(&g_lock);
		g_native_window = window;
		g_window_ready = true;
		// The surface callbacks that would deliver the size do not fire on
		// this system, so set a sensible landscape default for SDL.
		if (g_surface_width == 0 || g_surface_height == 0)
		{
			/* SDL window size = game logical resolution. */
			g_surface_width = 1920;
			g_surface_height = 1080;
		}
		pthread_cond_broadcast(&g_cond);
		pthread_mutex_unlock(&g_lock);
	}
}


void OHOS_Entry_SetVideoSurfaceId(const char *surface_id)
{
	if (surface_id == nullptr || surface_id[0] == '\0')
	{
		return;
	}
	uint64_t id = 0;
	try
	{
		id = static_cast<uint64_t>(std::strtoull(surface_id, nullptr, 10));
	}
	catch (...)
	{
		return;
	}

	OHNativeWindow *window = nullptr;
	int32_t ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId(id, &window);
	if (ret == 0 && window != nullptr)
	{
		pthread_mutex_lock(&g_lock);
		if (g_video_native_window != nullptr)
		{
			OH_NativeWindow_DestroyNativeWindow(g_video_native_window);
		}
		g_video_native_window = window;
		pthread_mutex_unlock(&g_lock);
	}
}

void OHOS_Entry_SetSurfaceSize(uint64_t width, uint64_t height)
{
	if (width == 0 || height == 0) return;
	pthread_mutex_lock(&g_lock);
	g_physical_width = width;
	g_physical_height = height;
	pthread_mutex_unlock(&g_lock);
	/* NOTE: g_surface_width/height (the SDL window size) stay at the game
	 * logical resolution 1920x1080. The physical size is recorded here only
	 * to scale touch coordinates from the XComponent pixel space into the
	 * logical window space. */
}

void OHOS_Entry_SetExternalDirs(const char *base_dir, const char *save_dir)
{
	SDL_OHOS_SetDataDir(base_dir);
	SDL_OHOS_SetSaveDir(save_dir);
	if (save_dir != nullptr && save_dir[0] != '\0') { setenv("KRKR_OHOS_SAVE_DIR", save_dir, 1); }
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
	// NOTE: DispatchTouchEvent intentionally NOT registered: on this system
	// libace_compatible crashes the UI thread while dispatching XComponent
	// touches to native callbacks (jump to 0xffffff8000000000, never reaching
	// OnTouchEvent). Touches are captured in ArkTS (.onTouch) and forwarded
	// through napi sendTouch instead.
	// callback.DispatchTouchEvent = OnTouchEvent;
	OH_NativeXComponent_RegisterCallback(native, &callback);

	// Mouse wheel / axis events (API 12): raw scroll deltas.
	OH_NativeXComponent_RegisterUIInputEventCallback(native, OnUIInputEvent,
		ARKUI_UIINPUTEVENT_TYPE_AXIS);

	// NOTE: do NOT call OH_NativeXComponent_GetXComponentSize with a null
	// window here: on this system that triggers SIGBUS on the UI thread.
	// The surface callbacks deliver the window; we wait for them instead.
}

static void EngineMain()
{
	RegisterSystemMouseMonitor();
	g_engine_running = true;
	OHOS_InstallCrashHandler();
	OHOS_DiagLog("EngineMain begin");

	if (!SDL_OHOS_WaitForNativeWindow(60000))
	{
		OHOS_DiagLog("WaitForNativeWindow TIMEOUT");
		g_engine_running = false;
		return;
	}
	OHOS_DiagLog("native window ready");

	// Determine the engine base directory. The ArkTS shell may have set an
	// external (public Download) base with game data at <base>/data and
	// savedata at <save>/. Fall back to the sandbox files directory.
	std::string base_dir = !g_data_dir.empty() ? g_data_dir : g_files_dir;
	bool data_ok = false;
	if (!base_dir.empty())
	{
		// Mirrors the Windows krkrz layout: the engine reads from the app
		// root, so data.xp3 sits at <base>/data.xp3 and loose files live in
		// <base>/data/.
		struct stat st;
		std::string startup = base_dir + "/data/startup.tjs";
		std::string system_dir = base_dir + "/data/system";
		std::string xp3 = base_dir + "/data.xp3";
		bool has_startup = stat(startup.c_str(), &st) == 0 && S_ISREG(st.st_mode);
		bool has_system = stat(system_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
		bool has_xp3 = stat(xp3.c_str(), &st) == 0 && S_ISREG(st.st_mode);
		data_ok = (has_startup && has_system) || has_xp3;
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
		g_engine_running = false;
		return;
	}

	if (!base_dir.empty())
		chdir(base_dir.c_str());

	if (!base_dir.empty())
	{
		// Let the engine core find the data without a cross-library symbol:
		// the environment survives the libentry -> libkrkrsdl2 boundary.
		setenv("KRKR_OHOS_DATA_DIR", base_dir.c_str(), 1);
	}

	char app_name[] = "krkrsdl2";
	char *argv[] = {app_name, nullptr};

	try
	{
		OHOS_DiagLog("pre_init enter");
		krkrsdl2_pre_init_platform();
		OHOS_DiagLog("pre_init done");
		krkrsdl2_convert_set_args(1, argv);
		OHOS_DiagLog("init_platform enter");
		if (krkrsdl2_init_platform())
		{
			// The application asked to terminate during startup.
			OHOS_DiagLog("init_platform requested termination");
			g_engine_running = false;
			return;
		}
		OHOS_DiagLog("init_platform done, entering main loop");
		krkrsdl2_run_main_loop();
		OHOS_DiagLog("main loop exited");
		krkrsdl2_cleanup();
	}
	catch (...)
	{
		OHOS_DiagLog("UNCAUGHT exception in engine");
	}

	OHOS_DiagLog("EngineMain end");
	g_engine_running = false;
}


/* ------------------------------------------------------------------------- */
/* Video playback bridge (engine -> entry).                                  */
/* ------------------------------------------------------------------------- */
#include "ohos_video_player.h"

extern "C" {

/* EGL_EXT_device_enumeration / EGL_EXT_platform_base compatibility stubs.
 * The OpenHarmony system EGL does not expose these extensions, but SDL's
 * EGL module (SDL_egl.c) requires eglQueryDevicesEXT + eglGetPlatformDisplayEXT
 * for SDL_EGL_InitializeOffscreen() and fails with "eglQueryDevicesEXT is
 * missing". SDL resolves them with SDL_LoadFunction(NULL, ...) i.e.
 * dlsym(NULL, ...), which only sees symbols in the main program and globally
 * loaded libraries - so define them HERE in libentry.so (the main module) so
 * dlsym(NULL) finds them. They report a synthetic device that maps back to
 * eglGetDisplay(EGL_DEFAULT_DISPLAY). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
static EGLDeviceEXT g_ohos_synth_device = (EGLDeviceEXT)1;

EGLBoolean eglQueryDevicesEXT(EGLint max_devices, EGLDeviceEXT *devices, EGLint *num_devices)
{
	if (devices != NULL && max_devices >= 1)
	{
		devices[0] = g_ohos_synth_device;
	}
	if (num_devices != NULL)
	{
		*num_devices = 1;
	}
	return EGL_TRUE;
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native_display, const EGLint *attribs)
{
	(void)platform;
	(void)native_display;
	(void)attribs;
	return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static Yosuga::OHOSVideoPlayer g_ohos_player;
/* True while a video open is waiting for the video XComponent to mount and
 * deliver its surfaceId: OHOS_Entry_IsVideoPlaying() reports it so the ArkTS
 * shell raises/creates the video view BEFORE the window is needed. */
static std::atomic<bool> g_video_pending{false};

int OHOS_VideoOpen(const char *path, int loop)
{
	/* The AVPlayer renders into its OWN video XComponent surface, never
	 * into the game window: the engine framebuffer and the video would
	 * otherwise fight over one shared buffer queue (LockBuffer puts the
	 * window into CPU production mode and disturbs the AVPlayer's GPU
	 * video rendering - video froze on its first frame). */
	/* The video XComponent only mounts while videoOnTop is true, which the
	 * ArkTS shell drives from OHOS_Entry_IsVideoPlaying(). Set the pending
	 * flag first so the view mounts, then wait for its surfaceId to turn
	 * into a native window. */
	/* ALWAYS drop the previous video window first. When the last video
	 * ended, ArkUI unmounted the video XComponent and invalidated its
	 * surface, but the pointer cached here survives. Binding the AVPlayer
	 * to that stale window (or racing the fresh SetVideoSurfaceId with an
	 * open on the old window) leaves the video surface without a
	 * compositor consumer: the OP movie plays with audio but a black
	 * picture. Waiting for the freshly mounted video view to deliver a
	 * NEW surfaceId guarantees the AVPlayer binds to a live surface. */
	g_video_pending = true;
	pthread_mutex_lock(&g_lock);
	if (g_video_native_window != nullptr)
	{
		OH_NativeWindow_DestroyNativeWindow(g_video_native_window);
		g_video_native_window = nullptr;
	}
	pthread_mutex_unlock(&g_lock);
	OHNativeWindow *win = nullptr;
	for (int attempt = 0; attempt < 120; ++attempt)
	{
		pthread_mutex_lock(&g_lock);
		win = g_video_native_window;
		pthread_mutex_unlock(&g_lock);
		if (win != nullptr)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	g_video_pending = false;
	if (win == nullptr) { return -1; }
	bool ok = g_ohos_player.Open(path ? path : "", win, loop != 0);
	return ok ? 0 : -1;
}

void OHOS_VideoStop(void)
{
	g_ohos_player.Stop();
}

void OHOS_VideoPause(void)
{
	g_ohos_player.Pause();
}

void OHOS_VideoResume(void)
{
	g_ohos_player.Resume();
}

void OHOS_VideoClose(void)
{
	g_ohos_player.Close();
}

void OHOS_VideoSetVolume(float vol)
{
	(void)vol;
}

void OHOS_VideoSetEndCallback(Yosuga::OHOSVideoPlayer::EndCallback cb)
{
	Yosuga::OHOSVideoPlayer::SetEndCallback(cb);
}

} // extern "C"

/* sdl_ohos_bridge.h: physical XComponent pixel size (for touch scaling). */
extern "C" int SDL_OHOS_GetPhysicalSize(int *width, int *height)
{
	pthread_mutex_lock(&g_lock);
	uint64_t w = g_physical_width, h = g_physical_height;
	pthread_mutex_unlock(&g_lock);
	if (width) *width = static_cast<int>(w);
	if (height) *height = static_cast<int>(h);
	return (w > 0 && h > 0) ? 1 : 0;
}

/* sdl_ohos_bridge.h: is the AVPlayer currently rendering into the surface?
 * The SDL renderer must pause while video plays (they share the XComponent
 * native window; the last Present wins). */
extern "C" int SDL_OHOS_IsVideoPlaying(void)
{
	return g_ohos_player.IsPlaying() ? 1 : 0;
}

/* Polled by the ArkTS shell to mount the video XComponent. True while the
 * AVPlayer renders AND while an open is waiting for the video surface, so
 * the view is created before the native window is needed. */
int OHOS_Entry_IsVideoPlaying(void)
{
	return (g_ohos_player.IsPlaying() || g_video_pending.load()) ? 1 : 0;
}

int OHOS_Entry_IsEngineRunning(void)
{
	/* Before the engine thread starts, report "running" so the ArkTS shell
	 * does not bounce back to the bootstrap page during startup. */
	if (!g_engine_started)
	{
		return 1;
	}
	return g_engine_running.load() ? 1 : 0;
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
