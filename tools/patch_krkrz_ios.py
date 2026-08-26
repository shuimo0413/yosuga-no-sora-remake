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

#if defined(__IPHONEOS__)
/* File-scope declarations (linkage specifications are not allowed at
 * block scope). */
extern "C" void krkrsdl2_ios_log(const char *);
extern bool TVPUtf16ToUtf8( std::string& out, const tjs_string& in );
#endif
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


if __name__ == "__main__":
    main()
