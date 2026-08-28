/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#ifndef __APPLICATION_SPECIAL_PATH_H__
#define __APPLICATION_SPECIAL_PATH_H__

#ifdef _WIN32
#include <shlobj.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "FilePathUtil.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__) && TARGET_OS_IPHONE
extern "C" const char *TVPIOSGetDocumentsDirectory(void);
#endif
#if defined(__ANDROID__)
extern "C" const char *TVPAndroidGetPublicSaveDirectory(void);
#endif

class ApplicationSpecialPath {
public:
#ifdef _WIN32
	static tjs_string GetSpecialFolderPath(int csidl) {
		tjs_char path[MAX_PATH+1];
		if(!SHGetSpecialFolderPath(NULL, path, csidl, false))
			return tjs_string();
		return tjs_string(path);
	}
	static inline tjs_string GetPersonalPath() {
		tjs_string path = GetSpecialFolderPath(CSIDL_PERSONAL);
		if( path.empty() ) path = GetSpecialFolderPath(CSIDL_APPDATA);

		if(path != TJS_W("")) {
			return path;
		}
		return TJS_W("");
	}
	static inline tjs_string GetAppDataPath() {
		tjs_string path = GetSpecialFolderPath(CSIDL_APPDATA);
		if(path != TJS_W("") ) {
			return path;
		}
		return TJS_W("");
	}
	static inline tjs_string GetSavedGamesPath() {
		tjs_string result;
		PWSTR ppszPath = NULL;
		HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &ppszPath);
		if( hr == S_OK ) {
			result = tjs_string( ppszPath );
			::CoTaskMemFree( ppszPath );
		}
		return result;
	}
#endif
#if 1
	static inline tjs_string ReplaceStringAll( tjs_string src, const tjs_string& target, const tjs_string& dest ) {
		tjs_string::size_type nPos = 0;
		while( (nPos = src.find(target, nPos)) != tjs_string::npos ) {
			src.replace( nPos, target.length(), dest );
		}
		return src;
	}
#endif

	static inline tjs_string GetConfigFileName( const tjs_string& exename ) {
		return ChangeFileExt(exename, TJS_W(".cf"));
	}
	static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
#ifdef _WIN32
		if(datapath == TJS_W("") ) datapath = tjs_string(TJS_W("$(exepath)\\savedata"));

		tjs_string exepath = ExcludeTrailingSlash(ExtractFileDir(exename));
		tjs_string personalpath = ExcludeTrailingSlash(GetPersonalPath());
		tjs_string appdatapath = ExcludeTrailingSlash(GetAppDataPath());
		tjs_string savedgamespath = ExcludeTrailingSlash(GetSavedGamesPath());
		if(personalpath == TJS_W("")) personalpath = exepath;
		if(appdatapath == TJS_W("")) appdatapath = exepath;
		if(savedgamespath == TJS_W("")) savedgamespath = exepath;

		datapath = ReplaceStringAll(datapath, TJS_W("$(exepath)"), exepath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(personalpath)"), personalpath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(appdatapath)"), appdatapath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(vistapath)"), appdatapath );
		datapath = ReplaceStringAll(datapath, TJS_W("$(savedgamespath)"), savedgamespath);
		return IncludeTrailingBackslash(ExpandUNCFileName(datapath));
#else
		if (datapath != TJS_W(""))
		{
			return datapath;
		}
#if defined(__APPLE__) && TARGET_OS_IPHONE
		/* iOS: keep saves in a game-specific subfolder of the app
		   <sandbox>/Documents (Documents/<bundle-id>/savedata) so the user
		   can reach, back up and re-import them through the Files app or
		   Finder (UIFileSharingEnabled) without risking a collision with
		   another application that shares the Documents directory.  Being
		   inside the sandbox it is never indexed into the system photo
		   library. */
		{
			const char *docs = TVPIOSGetDocumentsDirectory();
			tjs_string path;
			if(docs && *docs && TVPUtf8ToUtf16(path, std::string(docs)))
			{
				if(path.length() > 0 && path[path.length() - 1] != TJS_W('/'))
					path += TJS_W('/');
				return path;
			}
		}
		ttstr nativeDataPath = ttstr(TVPGetAppPath().AsStdString());
		nativeDataPath += TJS_W("/savedata/");
		return nativeDataPath.AsStdString();
#elif defined(__OHOS__)
		/* OHOS: saves live in the public Download app folder (set via
		   KRKR_OHOS_SAVE_DIR from the NAPI shell). */
		{
			const char *ohosSave = getenv("KRKR_OHOS_SAVE_DIR");
			if (ohosSave && *ohosSave)
			{
				tjs_string path;
				if (TVPUtf8ToUtf16(path, std::string(ohosSave)))
				{
					if (path.length() > 0 && path[path.length() - 1] != TJS_W('/'))
						path += TJS_W('/');
					return path;
				}
			}
		}
		/* OHOS fallback: use the public data dir env + savedata. */
		{
			const char *dd2 = getenv("KRKR_OHOS_DATA_DIR");
			if (dd2 && *dd2)
			{
				tjs_string p16;
				if (TVPUtf8ToUtf16(p16, std::string(dd2) + "/savedata/"))
					return p16;
			}
		}
		{
			char *pref_path = SDL_GetPrefPath(NULL, "krkrsdl2");
			if (pref_path)
			{
				std::string pp = pref_path;
				SDL_free(pref_path);
				tjs_string p16;
				if (TVPUtf8ToUtf16(p16, pp)) return p16;
			}
		}
		{
			ttstr ndp = ttstr(TVPGetAppPath().AsStdString());
			ndp += TJS_W("/savedata/");
			return ndp.AsStdString();
		}
#elif defined(__ANDROID__)
		/* Android: keep saves in the public Downloads folder. */
		{
			const char *androidDir = TVPAndroidGetPublicSaveDirectory();
			tjs_string path;
			if (androidDir && *androidDir &&
				TVPUtf8ToUtf16(path, std::string(androidDir)))
			{
				if (path.length() > 0 && path[path.length() - 1] != TJS_W('/'))
					path += TJS_W('/');
				return path;
			}
		}
		{
			char *pref_path = SDL_GetPrefPath(NULL, "krkrsdl2");
			std::string pref_path_utf8;
			if (pref_path)
			{
				pref_path_utf8 = pref_path;
				SDL_free(pref_path);
				tjs_string pref_path_utf16;
				TVPUtf8ToUtf16(pref_path_utf16, pref_path_utf8);
				return pref_path_utf16;
			}
			ttstr nativeDataPath = ttstr(TVPGetAppPath().AsStdString());
			TVPGetLocalName(nativeDataPath);
			nativeDataPath += TJS_W("/savedata/");
			return nativeDataPath.AsStdString();
		}
#elif defined(__vita__)
		return TJS_W("savedata0:/savedata/");
#else
		char *pref_path = SDL_GetPrefPath(NULL, "krkrsdl2");
		std::string pref_path_utf8;
		if (pref_path)
		{
			pref_path_utf8 = pref_path;
			SDL_free(pref_path);
			tjs_string pref_path_utf16;
			TVPUtf8ToUtf16(pref_path_utf16, pref_path_utf8);
			return pref_path_utf16;
		}
		ttstr nativeDataPath = ttstr(TVPGetAppPath().AsStdString());
#ifndef __EMSCRIPTEN__
		if (!nativeDataPath.IsEmpty())
		{
			TVPGetLocalName(nativeDataPath);
		}
#endif
		nativeDataPath += TJS_W("/savedata/");
		return nativeDataPath.AsStdString();
#endif
#endif
	}
	static tjs_string GetUserConfigFileName( const tjs_string& datapath, const tjs_string& exename ) {
		// exepath, personalpath, appdatapath
		return GetDataPathDirectory(datapath, exename) + ExtractFileName(ChangeFileExt(exename, TJS_W(".cfu")));
	}
};


#endif // __APPLICATION_SPECIAL_PATH_H__
