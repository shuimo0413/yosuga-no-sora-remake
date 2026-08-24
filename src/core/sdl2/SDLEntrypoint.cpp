/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#include "SDLApplication.h"
#include "SysInitImpl.h"
#ifdef USE_SDL_MAIN
#include <SDL_main.h>
#endif
#if defined(__IPHONEOS__)
#include <cstdlib>
#endif

#if defined(USE_SDL_MAIN)
extern "C" int SDL_main(int argc, char **argv)
#elif defined(_WIN32) && defined(_UNICODE)
extern "C" int wmain(int argc, wchar_t **argv)
#else
extern "C" int main(int argc, char **argv)
#endif
{
	try
	{
		krkrsdl2_pre_init_platform();

#if defined(_WIN32) && defined(_UNICODE)
		krkrsdl2_set_args(argc, argv);
#else
		krkrsdl2_convert_set_args(argc, argv);
#endif

		if (krkrsdl2_init_platform())
		{
			TVPTerminateCode = 0;
			return TVPTerminateCode;
		}

		krkrsdl2_run_main_loop();

#ifndef __EMSCRIPTEN__
		krkrsdl2_cleanup();
#endif

#if defined(__IPHONEOS__)
		/* iOS has no programmatic way to close an app except exit(): the
		 * in-game exit button ends the engine loop, and returning here would
		 * just leave a frozen SDL frame on screen. Terminate the process. */
		exit(TVPTerminateCode);
#endif
	}
	catch (...)
	{
		TVPTerminateCode = 2;
		return TVPTerminateCode;
	}
#ifdef _WIN32
	::TerminateProcess(::GetCurrentProcess(), (UINT)TVPTerminateCode);
#endif
	return TVPTerminateCode;
}

