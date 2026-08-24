/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "tjsCommHead.h"

#include "SysInitIntf.h"
#include "AudioDevice.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#ifdef __OHOS__
#include <dlfcn.h>
static const char *OHOSACSandboxDir(void)
{
	static const char *(*fn)(void) = nullptr;
	static const char *cached = nullptr;
	if (cached)
		return cached;
	if (fn == nullptr)
	{
		void *handle = dlopen("libentry.so", RTLD_NOW);
		if (!handle)
			handle = RTLD_DEFAULT;
		fn = (const char *(*)(void))dlsym(handle, "SDL_OHOS_GetFilesDir");
	}
	if (fn)
		cached = fn();
	return cached;
}
static void OHOSACLog(const char *line)
{
	const char *dirs[2];
	int ndirs = 0;
	const char *sandbox = OHOSACSandboxDir();
	const char *pub = getenv("KRKR_OHOS_DATA_DIR");
	if (sandbox && sandbox[0])
		dirs[ndirs++] = sandbox;
	if (pub && pub[0])
		dirs[ndirs++] = pub;
	for (int i = 0; i < ndirs; ++i)
	{
		std::string lpath = std::string(dirs[i]) + "/engine.log";
		FILE *lf = fopen(lpath.c_str(), "a");
		if (lf)
		{
			fprintf(lf, "audio: %s\n", line);
			fclose(lf);
		}
	}
}
#endif

// Defined in NullAudioDevice.cpp
extern iTVPAudioDevice* TVPCreateAudioDevice_Null();

// Defined in FAudioDevice.cpp
extern iTVPAudioDevice* TVPCreateAudioDevice_FAudio();

enum tTVPAudioDeviceChoice
{
	adcAuto,
	adcNull,
#ifdef TVP_FAUDIO_IMPLEMENT
	adcFAudio,
#endif
};

static tjs_int TVPAudioDeviceOptionsGeneration = 0;
static tTVPAudioDeviceChoice TVPAudioDeviceChoice = adcAuto;

static void TVPInitAudioDeviceOptions()
{
	if (TVPAudioDeviceOptionsGeneration == TVPGetCommandLineArgumentGeneration())
	{
		return;
	}
	TVPAudioDeviceOptionsGeneration = TVPGetCommandLineArgumentGeneration();

	tTJSVariant val;
	TVPAudioDeviceChoice = adcAuto;
	if (TVPGetCommandLine(TJS_W("-audiodevice"), &val))
	{
		ttstr str(val);
		if (str == TJS_W("null"))
			TVPAudioDeviceChoice = adcNull;
#ifdef TVP_FAUDIO_IMPLEMENT
		else if (str == TJS_W("faudio"))
			TVPAudioDeviceChoice = adcFAudio;
#endif
	}
}

iTVPAudioDevice* TVPCreateAudioDevice()
{
	iTVPAudioDevice* device = nullptr;

	TVPInitAudioDeviceOptions();
#ifdef TVP_FAUDIO_IMPLEMENT
	if ((device == nullptr) && (TVPAudioDeviceChoice == adcFAudio || TVPAudioDeviceChoice == adcAuto))
	{
		device = TVPCreateAudioDevice_FAudio();
#ifdef __OHOS__
		{
			char line[96];
			snprintf(line, sizeof(line), "TVPCreateAudioDevice_FAudio -> %s",
				device ? "device created" : "NULL");
			OHOSACLog(line);
		}
#endif
	}
#endif
	if ((device == nullptr) && (TVPAudioDeviceChoice == adcNull || TVPAudioDeviceChoice == adcAuto))
	{
		device = TVPCreateAudioDevice_Null();
#ifdef __OHOS__
		{
			OHOSACLog("falling back to NULL audio device (no sound)");
		}
#endif
	}
	return device;
}

