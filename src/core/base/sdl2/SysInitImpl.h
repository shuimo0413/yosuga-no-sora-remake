//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// System Initialization and Uninitialization
//---------------------------------------------------------------------------
#ifndef SysInitImplH
#define SysInitImplH

//---------------------------------------------------------------------------
extern void TVPDumpHWException();

extern void TVPInitializeBaseSystems();

// Load external patch archives (patch.xp3, ...) from the public data folder.
extern void TVPLoadExternalPatchArchives(const char *saveDirUtf8);

extern tjs_string TVPNativeProjectDir;
extern tjs_string TVPNativeDataPath;

extern bool TVPProjectDirSelected;
extern void TVPEnsureDataPathDirectory();


extern bool TVPExecuteUserConfig();

extern bool TVPTerminated;
extern bool TVPTerminateOnWindowClose;
extern bool TVPTerminateOnNoWindowStartup;
extern int TVPTerminateCode;

//---------------------------------------------------------------------------


#include "SysInitIntf.h"

#endif
