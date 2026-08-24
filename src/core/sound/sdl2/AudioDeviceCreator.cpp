/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "tjsCommHead.h"

#include "SysInitIntf.h"
#include "AudioDevice.h"
#include <cstdio>
#include <cstdlib>
#include <string>

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
			const char *pub = getenv("KRKR_OHOS_DATA_DIR");
			if (pub && pub[0])
			{
				std::string lpath = std::string(pub) + "/engine.log";
				FILE *lf = fopen(lpath.c_str(), "a");
				if (lf)
				{
					fprintf(lf, "audio: TVPCreateAudioDevice_FAudio -> %s
",
						device ? "device created" : "NULL");
					fclose(lf);
				}
			}
		}
#endif
	}
#endif
	if ((device == nullptr) && (TVPAudioDeviceChoice == adcNull || TVPAudioDeviceChoice == adcAuto))
	{
		device = TVPCreateAudioDevice_Null();
#ifdef __OHOS__
		{
			const char *pub = getenv("KRKR_OHOS_DATA_DIR");
			if (pub && pub[0])
			{
				std::string lpath = std::string(pub) + "/engine.log";
				FILE *lf = fopen(lpath.c_str(), "a");
				if (lf)
				{
					fprintf(lf, "audio: falling back to NULL audio device (no sound)
");
					fclose(lf);
				}
			}
		}
#endif
	}
	return device;
}

