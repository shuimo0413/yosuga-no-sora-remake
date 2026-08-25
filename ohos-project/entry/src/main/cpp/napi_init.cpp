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
#include <string>

#include <atomic>
#include <thread>

#include "krkrsdl2_ohos_entry.h"
#include "sdl_ohos_bridge.h"
#include "ohos_xp3_extract.h"

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

struct Xp3ExtractContext {
  napi_env env = nullptr;
  napi_deferred deferred = nullptr;
  napi_threadsafe_function tsfn = nullptr;
  std::thread worker;
  OHOSXp3ExtractResult result{};
  std::string xp3Path;
  std::string outDir;
  std::atomic<int> lastProgress{0};
  int rc = -1;
};

struct Xp3ProgressPayload {
  int done;
  int total;
  int final; /* 1: terminal call - resolves the promise */
  int rc;
  char name[512];
  char error[512];
};

void Xp3ResolveFinal(napi_env env, Xp3ExtractContext *ctx,
  Xp3ProgressPayload *payload) {
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value okVal = nullptr;
  napi_get_boolean(env, payload->rc == 0, &okVal);
  napi_set_named_property(env, obj, "ok", okVal);
  if (payload->rc == 0) {
    napi_value done = nullptr, total = nullptr;
    napi_create_int32(env, payload->done, &done);
    napi_create_int32(env, payload->total, &total);
    napi_set_named_property(env, obj, "filesDone", done);
    napi_set_named_property(env, obj, "filesTotal", total);
  } else {
    napi_value err = nullptr;
    napi_create_string_utf8(env, payload->error, NAPI_AUTO_LENGTH, &err);
    napi_set_named_property(env, obj, "error", err);
  }
  napi_resolve_deferred(env, ctx->deferred, obj);
}

void Xp3CallJs(napi_env env, napi_value jsCb, void *context, void *data) {
  Xp3ExtractContext *ctx = static_cast<Xp3ExtractContext *>(context);
  Xp3ProgressPayload *payload = static_cast<Xp3ProgressPayload *>(data);
  if (payload->final) {
    Xp3ResolveFinal(env, ctx, payload);
  } else if (jsCb != nullptr) {
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_create_int32(env, payload->done, &args[0]);
    napi_create_int32(env, payload->total, &args[1]);
    napi_create_string_utf8(env, payload->name, NAPI_AUTO_LENGTH, &args[2]);
    napi_value global = nullptr;
    napi_get_global(env, &global);
    napi_value ignored = nullptr;
    if (napi_call_function(env, global, jsCb, 3, args, &ignored) != napi_ok) {
      /* drop any pending JS exception from the progress callback */
      napi_value exc = nullptr;
      napi_get_and_clear_last_exception(env, &exc);
    }
  }
  delete payload;
}

void Xp3TsfnFinalize(napi_env env, void *data, void *context) {
  Xp3ExtractContext *ctx = static_cast<Xp3ExtractContext *>(context);
  if (ctx->worker.joinable()) {
    ctx->worker.join(); /* worker finished before the tsfn was released */
  }
  delete ctx;
}

int Xp3ProgressBridge(void *vctx, int done, int total, const char *nameUtf8) {
  Xp3ExtractContext *ctx = static_cast<Xp3ExtractContext *>(vctx);
  /* Throttle: at most one UI update per 128 extracted files. */
  if (done - ctx->lastProgress.load() < 128 && done < total) {
    return 1;
  }
  ctx->lastProgress.store(done);
  Xp3ProgressPayload *payload = new Xp3ProgressPayload();
  payload->done = done;
  payload->total = total;
  payload->final = 0;
  payload->rc = 0;
  payload->name[0] = '\0';
  if (nameUtf8) {
    strncpy(payload->name, nameUtf8, sizeof(payload->name) - 1);
    payload->name[sizeof(payload->name) - 1] = '\0';
  }
  napi_call_threadsafe_function(ctx->tsfn, payload, napi_tsfn_blocking);
  return 1;
}

void Xp3WorkerMain(void *vctx) {
  Xp3ExtractContext *ctx = static_cast<Xp3ExtractContext *>(vctx);
  OHOSXp3ExtractResult res;
  int rc = OHOS_ExtractXp3(ctx->xp3Path.c_str(), ctx->outDir.c_str(),
    Xp3ProgressBridge, ctx, &res);
  ctx->rc = rc;
  ctx->result = res;
  Xp3ProgressPayload *payload = new Xp3ProgressPayload();
  payload->done = res.filesDone;
  payload->total = res.filesTotal;
  payload->final = 1;
  payload->rc = rc;
  payload->name[0] = '\0';
  snprintf(payload->error, sizeof(payload->error), "%s", res.error);
  napi_call_threadsafe_function(ctx->tsfn, payload, napi_tsfn_blocking);
  /* Release our reference so the JS thread finalizes the tsfn (and deletes
   * the context) once the final payload has been processed. */
  napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
}

} /* namespace */

/* Resolve the deferred promise with {ok:false, error:msg} instead of
 * throwing: an ArkTS exception loses its message across the NAPI boundary
 * (the UI only ever shows "{}" then). */
static napi_value Xp3ResolveError(napi_env env, napi_deferred deferred,
  const char *msg) {
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value okVal = nullptr;
  napi_get_boolean(env, false, &okVal);
  napi_set_named_property(env, obj, "ok", okVal);
  napi_value err = nullptr;
  napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &err);
  napi_set_named_property(env, obj, "error", err);
  napi_resolve_deferred(env, deferred, obj);
  return obj;
}

/* extractXp3(xp3Path, outDir, progressCb?): Promise<{ok, filesDone,
 * filesTotal, error?}>. The extraction runs on a native worker thread;
 * progressCb(done, total, name) is throttled and invoked on the JS thread.
 * Every failure resolves the promise with an error object - never throws -
 * so the ArkTS side can display the reason. */
static napi_value ExtractXp3(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 2) {
    napi_throw_type_error(env, nullptr,
      "extractXp3 expects (xp3Path, outDir[, progressCb])");
    return nullptr;
  }

  Xp3ExtractContext *ctx = new Xp3ExtractContext();
  ctx->env = env;

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

  napi_value promise = nullptr;
  napi_create_promise(env, &ctx->deferred, &promise);

  if (ctx->xp3Path.empty() || ctx->outDir.empty()) {
    Xp3ResolveError(env, ctx->deferred, "extractXp3: empty path argument");
    delete ctx;
    return promise;
  }

  napi_value jsCb = nullptr;
  if (argc >= 3) {
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[2], &type);
    if (type == napi_function) {
      jsCb = args[2];
    }
  }

  napi_status tsfnStatus = napi_create_threadsafe_function(env, jsCb, nullptr,
    nullptr, 0, 1, nullptr, Xp3TsfnFinalize, ctx, Xp3CallJs, &ctx->tsfn);
  if (tsfnStatus != napi_ok) {
    char buf[160];
    snprintf(buf, sizeof(buf),
      "extractXp3: cannot create worker callback (status %d)", (int)tsfnStatus);
    Xp3ResolveError(env, ctx->deferred, buf);
    delete ctx;
    return promise;
  }

  try {
    ctx->worker = std::thread(Xp3WorkerMain, ctx);
  } catch (...) {
    Xp3ResolveError(env, ctx->deferred,
      "extractXp3: cannot start worker thread");
    /* releasing the tsfn triggers its finalize, which deletes ctx */
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
    return promise;
  }
  return promise;
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
		{"setVideoSurfaceId", nullptr, SetVideoSurfaceId, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"isVideoPlaying", nullptr, IsVideoPlaying, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"isEngineRunning", nullptr, IsEngineRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setSurfaceSize", nullptr, SetSurfaceSize, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"extractXp3", nullptr, ExtractXp3, nullptr, nullptr, nullptr, napi_default, nullptr},
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
