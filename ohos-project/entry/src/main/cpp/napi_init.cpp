/* SPDX-License-Identifier: MIT */
/*
 * "entry" NAPI module. The ArkTS shell imports it as 'libentry.so' and calls
 * initResourceManager, onLoad and startEngine.
 */

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <napi/native_api.h>

#include <cstring>
#include <string>

#include "krkrsdl2_ohos_entry.h"

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
		napi_throw_error(env, nullptr, "onLoad expects the XComponent onLoad context");
		return nullptr;
	}

	// The XComponent onLoad context carries the OH_NativeXComponent pointer.
	OH_NativeXComponent *component = nullptr;
	napi_status status = napi_unwrap(env, args[0], reinterpret_cast<void **>(&component));
	if (status != napi_ok || component == nullptr)
	{
		napi_throw_error(env, nullptr, "onLoad: cannot unwrap the XComponent context");
		return nullptr;
	}
	OHOS_Entry_AttachXComponent(component);
	return nullptr;
}

static napi_value StartEngine(napi_env env, napi_callback_info info)
{
	OHOS_Entry_StartEngine();
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

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
	napi_property_descriptor desc[] = {
		{"initResourceManager", nullptr, InitResourceManager, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"onLoad", nullptr, OnLoad, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"startEngine", nullptr, StartEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setExternalDirs", nullptr, SetExternalDirs, nullptr, nullptr, nullptr, napi_default, nullptr},
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
