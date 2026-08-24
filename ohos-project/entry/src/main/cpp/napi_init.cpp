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

#include "krkrsdl2_ohos_entry.h"
#include "sdl_ohos_bridge.h"

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
	napi_valuetype value_type = napi_undefined;
	napi_typeof(env, args[0], &value_type);
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
				OHOS_Entry_LogNative("XComponent OnLoad: got component via __NATIVE_XCOMPONENT_OBJ__ external");
			}
			else if (napi_unwrap(env, native_obj, &raw) == napi_ok && raw != nullptr)
			{
				component = static_cast<OH_NativeXComponent *>(raw);
				OHOS_Entry_LogNative("XComponent OnLoad: got component via __NATIVE_XCOMPONENT_OBJ__ unwrap");
			}
			else
			{
				OHOS_Entry_LogNative("XComponent OnLoad: __NATIVE_XCOMPONENT_OBJ__ present but neither external nor unwrapable");
			}
		}
		else
		{
			OHOS_Entry_LogNative("XComponent OnLoad: __NATIVE_XCOMPONENT_OBJ__ property MISSING");
		}
	}

	// Dump the context object property names to understand its shape.
	std::string props = "?";
	if (value_type == napi_object)
	{
		napi_value names = nullptr;
		if (napi_get_property_names(env, args[0], &names) == napi_ok)
		{
			uint32_t len = 0;
			napi_get_array_length(env, names, &len);
			props = "";
			for (uint32_t i = 0; i < len && i < 24; ++i)
			{
				napi_value name = nullptr;
				napi_get_element(env, names, i, &name);
				char buf[128] = {0};
				size_t n = 0;
				if (napi_get_value_string_utf8(env, name, buf, sizeof(buf), &n) == napi_ok)
				{
					if (i > 0) props += ",";
					props += buf;
				}
			}
		}
	}

	std::string sid = "?";
	if (value_type == napi_object)
	{
		napi_value sid_val = nullptr;
		if (napi_get_named_property(env, args[0], "surfaceId", &sid_val) == napi_ok)
		{
			char buf[128] = {0};
			size_t n = 0;
			if (napi_get_value_string_utf8(env, sid_val, buf, sizeof(buf), &n) == napi_ok)
			{
				sid = buf;
			}
		}
	}

	char diagnostic[256];
	snprintf(diagnostic, sizeof(diagnostic),
		"XComponent OnLoad: type=%d unwrap=%d external=%d native=%s props=%s surfaceId=%s",
		static_cast<int>(value_type), static_cast<int>(unwrap_status),
		static_cast<int>(external_status), component != nullptr ? "yes" : "no",
		props.c_str(), sid.c_str());
	OHOS_Entry_LogNative(diagnostic);
	if (component == nullptr)
	{
		napi_value result = nullptr;
		napi_get_boolean(env, false, &result);
		return result;
	}
	OHOS_Entry_AttachXComponent(component);
	OHOS_Entry_LogNative("XComponent OnLoad: attached");
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
