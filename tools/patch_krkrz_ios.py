#!/usr/bin/env python3
"""Patch external/krkrz for iOS startup-script diagnostics (idempotent).

The krkrz submodule cannot take pushes from the main repository account,
so engine-side logging that lives in krkrz's ScriptMgnIntf.cpp is applied
here, on the CI runner, right after the submodules are checked out.
Applying the patch twice is a no-op.

Changes:
  1. TVPExecuteStorage logs the script name into bootstrap.log before
     executing it, so a hang inside the startup sequence can be located
     without a crash report.
  2. TVPExecuteStartupScript logs after startup.tjs finishes.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KRKRZ = REPO / "external" / "krkrz"
TARGET = KRKRZ / "base" / "ScriptMgnIntf.cpp"

MARKER = "#if defined(__IPHONEOS__)"

BLOCK0_OLD = """#include "tjsCommHead.h"
#include <string>
"""

BLOCK0_NEW = """#include "tjsCommHead.h"
#include <string>

/* File-scope declarations (linkage specifications are not allowed at
 * block scope). Plain extern declarations produce no symbols, so they are
 * harmless on platforms where krkrsdl2_ios_log is never called. */
extern "C" void krkrsdl2_ios_log(const char *);
extern bool TVPUtf16ToUtf8( std::string& out, const tjs_string& in );
"""

BLOCK1_OLD = """void TVPExecuteStorage(const ttstr &name, iTJSDispatch2 *context, tTJSVariant *result, bool isexpression,
	const tjs_char * modestr)
{
	// execute storage which contains script
	if(!TVPScriptEngine) TVPThrowInternalError;
"""

BLOCK1_NEW = """void TVPExecuteStorage(const ttstr &name, iTJSDispatch2 *context, tTJSVariant *result, bool isexpression,
	const tjs_char * modestr)
{
#if defined(__IPHONEOS__)
	{
		std::string n8;
		if (TVPUtf16ToUtf8(n8, name.AsStdString()))
			krkrsdl2_ios_log(("script: executing " + n8).c_str());
	}
#endif
	// execute storage which contains script
	if(!TVPScriptEngine) TVPThrowInternalError;
"""

BLOCK4_OLD = """	{ // for bytecode
		ttstr place(TVPSearchPlacedPath(name));
		ttstr shortname(TVPExtractStorageName(place));
		tTJSBinaryStream* stream = TVPCreateBinaryStreamForRead(place, modestr);
"""

BLOCK4_NEW = """	{ // for bytecode
#if defined(__IPHONEOS__)
		krkrsdl2_ios_log("script: searching placed path");
#endif
		ttstr place(TVPSearchPlacedPath(name));
#if defined(__IPHONEOS__)
		krkrsdl2_ios_log("script: placed path found");
#endif
		ttstr shortname(TVPExtractStorageName(place));
#if defined(__IPHONEOS__)
		krkrsdl2_ios_log("script: creating binary stream");
#endif
		tTJSBinaryStream* stream = TVPCreateBinaryStreamForRead(place, modestr);
"""

BLOCK5_OLD = """				isbytecode = TVPScriptEngine->LoadByteCode( stream, result, context, shortname.c_str() );
"""

BLOCK5_NEW = """#if defined(__IPHONEOS__)
				krkrsdl2_ios_log("script: loading bytecode");
#endif
				isbytecode = TVPScriptEngine->LoadByteCode( stream, result, context, shortname.c_str() );
#if defined(__IPHONEOS__)
				krkrsdl2_ios_log("script: bytecode load done");
#endif
"""

BLOCK6_OLD = """	ttstr place(TVPSearchPlacedPath(name));
	ttstr shortname(TVPExtractStorageName(place));

	iTJSTextReadStream * stream = TVPCreateTextStreamForReadByEncoding(place, modestr,TVPScriptTextEncoding);
"""

BLOCK6_NEW = """#if defined(__IPHONEOS__)
	krkrsdl2_ios_log("script: searching text path");
#endif
	ttstr place(TVPSearchPlacedPath(name));
	ttstr shortname(TVPExtractStorageName(place));

	iTJSTextReadStream * stream = TVPCreateTextStreamForReadByEncoding(place, modestr,TVPScriptTextEncoding);
#if defined(__IPHONEOS__)
	krkrsdl2_ios_log("script: text stream created");
#endif
"""

BLOCK3_OLD = """	if(TVPScriptEngine)
	{
		if(!isexpression)
			TVPScriptEngine->ExecScript(buffer, result, context,
				&shortname);
		else
			TVPScriptEngine->EvalExpression(buffer, result, context,
				&shortname);
	}
"""

BLOCK3_NEW = """	if(TVPScriptEngine)
	{
#if defined(__IPHONEOS__)
		krkrsdl2_ios_log("script: compiling & executing");
#endif
		if(!isexpression)
			TVPScriptEngine->ExecScript(buffer, result, context,
				&shortname);
		else
			TVPScriptEngine->EvalExpression(buffer, result, context,
				&shortname);
#if defined(__IPHONEOS__)
		krkrsdl2_ios_log("script: executed");
#endif
	}
"""

BLOCK2_OLD = """			TVPAddLog( TVPInfoLoadingStartupScript + TVPStartupScriptName );
			TVPExecuteStorage(TVPStartupScriptName);
			TVPAddLog( (const tjs_char*)TVPInfoStartupScriptEnded );
"""

BLOCK2_NEW = """			TVPAddLog( TVPInfoLoadingStartupScript + TVPStartupScriptName );
			TVPExecuteStorage(TVPStartupScriptName);
#if defined(__IPHONEOS__)
			krkrsdl2_ios_log("script: startup.tjs executed");
#endif
			TVPAddLog( (const tjs_char*)TVPInfoStartupScriptEnded );
"""


def apply(path: Path, old: str, new: str, what: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        print(f"already patched: {what}")
        return
    if old in text:
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        print(f"patched: {what}")
        return
    print(f"ERROR: patch target not found: {what}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if not TARGET.exists():
        print(f"ERROR: {TARGET} not found (run after submodule checkout)",
              file=sys.stderr)
        sys.exit(1)
    apply(TARGET, BLOCK0_OLD, BLOCK0_NEW, "file-scope declarations")
    apply(TARGET, BLOCK1_OLD, BLOCK1_NEW, "TVPExecuteStorage diag")
    apply(TARGET, BLOCK2_OLD, BLOCK2_NEW, "TVPExecuteStartupScript diag")
    apply(TARGET, BLOCK3_OLD, BLOCK3_NEW, "ExecScript boundaries diag")
    apply(TARGET, BLOCK4_OLD, BLOCK4_NEW, "bytecode path steps diag")
    apply(TARGET, BLOCK5_OLD, BLOCK5_NEW, "bytecode load diag")
    apply(TARGET, BLOCK6_OLD, BLOCK6_NEW, "text path steps diag")


if __name__ == "__main__":
    main()
