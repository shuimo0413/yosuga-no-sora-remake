/* SPDX-License-Identifier: MIT */
/*
 * "entry" NAPI module. The ArkTS shell imports it as 'libentry.so' and calls
 * initResourceManager, onLoad and startEngine.
 */

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <vector>
#include <napi/native_api.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include <filemanagement/file_uri/oh_file_uri.h>

#include <atomic>
#include <thread>

#include "krkrsdl2_ohos_entry.h"
#include "sdl_ohos_bridge.h"
#include "xp3_extract.h"

static napi_value InitResourceManager(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 2)
	{
		napi_throw_error(env, nullptr, "initResourceManager expects (abilityContext, filesDir)");
		return nullptr;
	}

	OHOS_Entry_SetResourceManager(env, args[0]);

	size_t length = 0;
	napi_get_value_string_utf8(env, args[1], nullptr, 0, &length);
	if (length > 0)
	{
		std::string files_dir(length, '\0');
		napi_get_value_string_utf8(env, args[1], &files_dir[0], length + 1, &length);
		OHOS_Entry_SetFilesDir(files_dir.c_str());
	}
	return nullptr;
}

static napi_value OnLoad(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 1)
	{
		return nullptr;
	}

	// The onLoad context carries the OH_NativeXComponent. Older systems wrap
	// it with napi_wrap (unwrap path); newer systems hand it out as a
	// napi external instead. Try all known paths and dump the object shape
	// so a failure is diagnosable. NEVER throw from here: on HarmonyOS 7 a
	// JS exception escapes and kills the app.
	OH_NativeXComponent *component = nullptr;
	napi_status unwrap_status = napi_unwrap(env, args[0], reinterpret_cast<void **>(&component));
	napi_status external_status = napi_invalid_arg;
	if (unwrap_status != napi_ok || component == nullptr)
	{
		void *external = nullptr;
		external_status = napi_get_value_external(env, args[0], &external);
		if (external_status == napi_ok && external != nullptr)
		{
			component = static_cast<OH_NativeXComponent *>(external);
		}
	}

	// API 26: the real OH_NativeXComponent is hidden in the
	// __NATIVE_XCOMPONENT_OBJ__ property of the onLoad context object.
	if (component == nullptr)
	{
		napi_value native_obj = nullptr;
		if (napi_get_named_property(env, args[0], "__NATIVE_XCOMPONENT_OBJ__", &native_obj) == napi_ok)
		{
			void *raw = nullptr;
			// It may be a plain external or a napi_wrap'ed object.
			if (napi_get_value_external(env, native_obj, &raw) == napi_ok && raw != nullptr)
			{
				component = static_cast<OH_NativeXComponent *>(raw);
			}
			else if (napi_unwrap(env, native_obj, &raw) == napi_ok && raw != nullptr)
			{
				component = static_cast<OH_NativeXComponent *>(raw);
			}
		}
	}

	if (component == nullptr)
	{
		napi_value result = nullptr;
		napi_get_boolean(env, false, &result);
		return result;
	}
	OHOS_Entry_AttachXComponent(component);
	napi_value ok = nullptr;
	napi_get_boolean(env, true, &ok);
	return ok;
}

static napi_value StartEngine(napi_env env, napi_callback_info info)
{
	OHOS_Entry_StartEngine();
	return nullptr;
}

static napi_value SetSurfaceId(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	size_t length = 0;
	if (argc < 1 ||
		napi_get_value_string_utf8(env, args[0], nullptr, 0, &length) != napi_ok)
	{
		return nullptr;
	}
	std::string surface_id(length, '\0');
	napi_get_value_string_utf8(env, args[0], &surface_id[0], length + 1, &length);
	OHOS_Entry_SetSurfaceId(surface_id.c_str());
	return nullptr;
}

/* setSurfaceSize(width, height): tell the engine the real XComponent pixel
 * size (the native OnSurfaceChanged callback never fires on this system). */
static napi_value SetSurfaceSize(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 2) return nullptr;
	int64_t w = 0, h = 0;
	if (napi_get_value_int64(env, args[0], &w) != napi_ok ||
		napi_get_value_int64(env, args[1], &h) != napi_ok) return nullptr;
	OHOS_Entry_SetSurfaceSize(static_cast<uint64_t>(w), static_cast<uint64_t>(h));
	return nullptr;
}

/* setExternalDirs(baseDir, saveDir): point the engine at the public Download
 * app folder (game data at <baseDir>/data) and the savedata folder. Either
 * argument may be an empty string or omitted. */
static napi_value SetExternalDirs(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

	std::string base_dir;
	std::string save_dir;
	for (size_t index = 0; index < 2; ++index)
	{
		napi_value value = index < argc ? args[index] : nullptr;
		size_t length = 0;
		if (value == nullptr ||
			napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok ||
			length == 0)
		{
			continue;
		}
		std::string text(length, '\0');
		napi_get_value_string_utf8(env, value, &text[0], length + 1, &length);
		if (index == 0)
		{
			base_dir = text;
		}
		else
		{
			save_dir = text;
		}
	}

	OHOS_Entry_SetExternalDirs(
		base_dir.empty() ? nullptr : base_dir.c_str(),
		save_dir.empty() ? nullptr : save_dir.c_str());
	return nullptr;
}

/* sendTouch(type, x, y): forward an ArkTS XComponent .onTouch() event to the
 * SDL OHOS backend. This path bypasses the libace_compatible XComponent
 * native-touch dispatch, which crashes the UI thread on this system
 * (DispatchTouchEvent never reaches the engine; the framework dies inside
 * libace_compatible first). */
static napi_value SendTouch(napi_env env, napi_callback_info info)
{
	size_t argc = 3;
	napi_value args[3] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 3)
	{
		return nullptr;
	}
	int32_t type = 0;
	double x = 0.0;
	double y = 0.0;
	if (napi_get_value_int32(env, args[0], &type) != napi_ok ||
		napi_get_value_double(env, args[1], &x) != napi_ok ||
		napi_get_value_double(env, args[2], &y) != napi_ok)
	{
		return nullptr;
	}
	SDL_OHOS_OnTouchEvent(static_cast<int>(type), static_cast<float>(x), static_cast<float>(y));
	return nullptr;
}

/* sendFinger(type, fingerId, x, y): forward ONE finger of an ArkTS touch
 * event as an SDL FINGER event. The engine turns a second finger press into
 * the right mouse button (skip movie / back out of menus), so every finger
 * must reach SDL - the old sendTouch only forwarded touches[0]. */
static napi_value SendFinger(napi_env env, napi_callback_info info)
{
	size_t argc = 4;
	napi_value args[4] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 4)
	{
		return nullptr;
	}
	int32_t type = 0;
	int32_t fingerId = 0;
	double x = 0.0;
	double y = 0.0;
	if (napi_get_value_int32(env, args[0], &type) != napi_ok ||
		napi_get_value_int32(env, args[1], &fingerId) != napi_ok ||
		napi_get_value_double(env, args[2], &x) != napi_ok ||
		napi_get_value_double(env, args[3], &y) != napi_ok)
	{
		return nullptr;
	}
	SDL_OHOS_OnFingerEvent(static_cast<int>(fingerId), static_cast<int>(type),
		static_cast<float>(x), static_cast<float>(y));
	return nullptr;
}

/* sendMouse(action, button, x, y): forward an ArkTS .onMouse() event to the
 * SDL OHOS backend. action 0 = down, 1 = up, 2 = wheel (x = vertical scroll
 * delta, y = horizontal scroll delta). button 1 = left, 2 = middle, 3 = right. */
static napi_value SendMouse(napi_env env, napi_callback_info info)
{
	size_t argc = 4;
	napi_value args[4] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 4)
	{
		return nullptr;
	}
	int32_t action = 0, button = 0;
	double x = 0.0, y = 0.0;
	if (napi_get_value_int32(env, args[0], &action) != napi_ok ||
		napi_get_value_int32(env, args[1], &button) != napi_ok ||
		napi_get_value_double(env, args[2], &x) != napi_ok ||
		napi_get_value_double(env, args[3], &y) != napi_ok)
	{
		return nullptr;
	}
	SDL_OHOS_OnMouseEvent(static_cast<int>(action), static_cast<int>(button),
		static_cast<int>(x), static_cast<int>(y));
	return nullptr;
}

/* sendKey(down, keycode): forward an ArkTS key event to the SDL OHOS
 * backend. keycode is the OHOS KeyCode; the backend maps it to a scancode. */
static napi_value SendKey(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value args[2] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 2)
	{
		return nullptr;
	}
	int32_t down = 0, keycode = 0;
	if (napi_get_value_int32(env, args[0], &down) != napi_ok ||
		napi_get_value_int32(env, args[1], &keycode) != napi_ok)
	{
		return nullptr;
	}
	SDL_OHOS_OnKeyEvent(static_cast<int>(down), static_cast<int>(keycode));
	return nullptr;
}

/* forceExit(): hard-terminate the process (terminateSelf leaves a black
 * window behind on some devices). */
static napi_value ForceExit(napi_env env, napi_callback_info info)
{
	(void)env;
	(void)info;
	/* Hard-exit: exit(0) may still run atexit/static destructors (SDL/
	 * engine cleanup) that can hang on KaihongOS. _exit skips them. */
	_exit(0);
	return nullptr;
}

/* setVideoSurfaceId(sid): create a SEPARATE native window from the video
 * XComponent's surfaceId; the AVPlayer renders into it. */
static napi_value SetVideoSurfaceId(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 1)
	{
		return nullptr;
	}
	size_t len = 0;
	napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
	std::vector<char> buf(len + 1, 0);
	napi_get_value_string_utf8(env, args[0], buf.data(), len + 1, &len);
	OHOS_Entry_SetVideoSurfaceId(buf.data());
	return nullptr;
}

/* isVideoPlaying(): polled by ArkTS to raise the video XComponent. */
static napi_value IsVideoPlaying(napi_env env, napi_callback_info info)
{
	(void)info;
	napi_value result;
	napi_get_boolean(env, OHOS_Entry_IsVideoPlaying() != 0, &result);
	return result;
}

/* isEngineRunning(): polled by ArkTS so the shell can return to the
 * bootstrap page after the game exits from its in-game menu. */
static napi_value IsEngineRunning(napi_env env, napi_callback_info info)
{
	(void)info;
	napi_value result;
	napi_get_boolean(env, OHOS_Entry_IsEngineRunning() != 0, &result);
	return result;
}

/* ---- data.xp3 extraction ------------------------------------------------ */

namespace {

/* The OHOS napi_threadsafe_function rejected our create call with
 * napi_invalid_arg, so the worker communicates with the ArkTS side through
 * two files next to the output directory (the same polling pattern the rest
 * of the shell already uses):
 *   <outDir>.progress : "done total name\n"        (rewritten ~every 128 files)
 *   <outDir>.status   : "ok done total\n" or "error <message>\n" (written once)
 * The worker thread owns and deletes its context when it finishes. */

struct Xp3ExtractContext {
  std::thread worker;
  std::string xp3Path;
  std::string outDir;
  std::string progressPath;
  std::string statusPath;
};

int Xp3ProgressBridge(void *vctx, int done, int total, const char *nameUtf8) {
  Xp3ExtractContext *ctx = static_cast<Xp3ExtractContext *>(vctx);
  /* Throttle: at most one file write per 512 extracted files (less FUSE
   * churn while thousands of files land in the public folder). */
  if (done % 512 != 0 && done < total) {
    return 1;
  }
  FILE *p = fopen(ctx->progressPath.c_str(), "w");
  if (p) {
    fprintf(p, "%d %d %s\n", done, total, nameUtf8 ? nameUtf8 : "");
    fclose(p);
  }
  return 1;
}

void Xp3WorkerMain(void *vctx) {
  Xp3ExtractContext *ctx = static_cast<Xp3ExtractContext *>(vctx);
  OHOSXp3ExtractResult res;
  memset(&res, 0, sizeof(res));
  int rc = -1;
  try {
    rc = OHOS_ExtractXp3(ctx->xp3Path.c_str(), ctx->outDir.c_str(),
      Xp3ProgressBridge, ctx, &res);
  } catch (...) {
    snprintf(res.error, sizeof(res.error), "worker thread exception");
  }
  FILE *s = fopen(ctx->statusPath.c_str(), "w");
  if (s) {
    if (rc == 0) {
      fprintf(s, "ok %d %d\n", res.filesDone, res.filesTotal);
    } else {
      fprintf(s, "error %s\n", res.error);
    }
    fclose(s);
  }
  /* Detach BEFORE deleting the context: ctx owns the std::thread object,
   * and deleting a joinable thread (this very thread) calls
   * std::terminate -> abort (signal 6 in musl). */
  ctx->worker.detach();
  delete ctx;
}

} /* namespace */

/* extractXp3Start(xp3Path, outDir): boolean. Starts the extraction on a
 * native worker thread and returns immediately. The ArkTS side polls
 * <outDir>.status / <outDir>.progress until the status file appears. */
static napi_value ExtractXp3Start(napi_env env, napi_callback_info info) {
  /* The extraction runs before the engine (which normally installs the
   * crash recorder), so install it here too: a worker crash must leave a
   * crash.txt behind instead of dying silently. */
  OHOS_InstallCrashHandler();
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    napi_throw_type_error(env, nullptr,
      "extractXp3Start expects (xp3Path, outDir)");
    return nullptr;
  }

  Xp3ExtractContext *ctx = new Xp3ExtractContext();

  size_t len = 0;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) == napi_ok &&
    len > 0) {
    ctx->xp3Path.resize(len);
    napi_get_value_string_utf8(env, args[0], &ctx->xp3Path[0], len + 1, &len);
  }
  len = 0;
  if (napi_get_value_string_utf8(env, args[1], nullptr, 0, &len) == napi_ok &&
    len > 0) {
    ctx->outDir.resize(len);
    napi_get_value_string_utf8(env, args[1], &ctx->outDir[0], len + 1, &len);
  }
  if (ctx->xp3Path.empty() || ctx->outDir.empty()) {
    delete ctx;
    napi_throw_type_error(env, nullptr, "extractXp3Start: empty path argument");
    return nullptr;
  }

  ctx->progressPath = ctx->outDir + ".progress";
  ctx->statusPath = ctx->outDir + ".status";
  /* clear stale state from an interrupted run */
  FILE *p = fopen(ctx->progressPath.c_str(), "w");
  if (p) fclose(p);
  FILE *s = fopen(ctx->statusPath.c_str(), "w");
  if (s) fclose(s);

  try {
    ctx->worker = std::thread(Xp3WorkerMain, ctx);
  } catch (...) {
    delete ctx;
    napi_value r = nullptr;
    napi_get_boolean(env, false, &r);
    return r;
  }
  napi_value r = nullptr;
  napi_get_boolean(env, true, &r);
  return r;
}

/* Convert a picker document URI (file://docs/...) to the real sandbox path
 * via the official OH_FileUri_GetPathFromUri. Returns undefined on failure. */
static napi_value UriToPath(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value args[1] = {nullptr};
	napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
	if (argc < 1)
	{
		return nullptr;
	}
	size_t length = 0;
	napi_get_value_string_utf8(env, args[0], nullptr, 0, &length);
	if (length == 0)
	{
		return nullptr;
	}
	std::string uri_str(length, '\0');
	napi_get_value_string_utf8(env, args[0], &uri_str[0], length + 1, &length);

	napi_value r = nullptr;
	// KaihongOS: the official conversion returns the virtualized
	// /storage/Users/... path that the app sandbox denies (13900012).
	// The granted folder is really mounted read-write inside the app's
	// sharefs view at /data/storage/el2/share/rw/docs/<path> (the
	// /data/service/el2/.../share path only exists in the root mount
	// namespace, not in the app's), so build that path directly for
	// file://docs/ uris.
	if (uri_str.compare(0, 12, "file://docs/") == 0)
	{
		char share[512];
		snprintf(share, sizeof(share),
			"/data/storage/el2/share/rw/docs/%s", uri_str.c_str() + 12);
		napi_create_string_utf8(env, share, NAPI_AUTO_LENGTH, &r);
		return r;
	}

	char *path = nullptr;
	FileManagement_ErrCode rc = OH_FileUri_GetPathFromUri(uri_str.c_str(), uri_str.size(), &path);
	if (rc == 0 && path != nullptr && path[0] != '\0')
	{
		napi_create_string_utf8(env, path, NAPI_AUTO_LENGTH, &r);
		free(path);
	}
	else
	{
		if (path != nullptr)
		{
			free(path);
		}
		napi_get_undefined(env, &r);
	}
	return r;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
	napi_property_descriptor desc[] = {
		{"initResourceManager", nullptr, InitResourceManager, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"onLoad", nullptr, OnLoad, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"startEngine", nullptr, StartEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setSurfaceId", nullptr, SetSurfaceId, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setExternalDirs", nullptr, SetExternalDirs, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"sendTouch", nullptr, SendTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"sendFinger", nullptr, SendFinger, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"sendMouse", nullptr, SendMouse, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"sendKey", nullptr, SendKey, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"forceExit", nullptr, ForceExit, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setVideoSurfaceId", nullptr, SetVideoSurfaceId, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"isVideoPlaying", nullptr, IsVideoPlaying, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"isEngineRunning", nullptr, IsEngineRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setSurfaceSize", nullptr, SetSurfaceSize, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"extractXp3Start", nullptr, ExtractXp3Start, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"uriToPath", nullptr, UriToPath, nullptr, nullptr, nullptr, napi_default, nullptr},
	};
	napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
	return exports;
}

static napi_module entry_module = {
	.nm_version = 1,
	.nm_flags = 0,
	.nm_filename = nullptr,
	.nm_register_func = Init,
	.nm_modname = "entry",
	.nm_priv = nullptr,
	.reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
	napi_module_register(&entry_module);
}
EXTERN_C_END
