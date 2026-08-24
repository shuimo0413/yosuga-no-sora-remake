#!/usr/bin/env python3
"""Patch external/krkrz for OHOS xp3 auto-path fixes (idempotent).

The krkrz submodule (LightWinder/krkrz) does not accept pushes from the
main repository account, so the engine changes that live in krkrz's
TVPRebuildAutoPathTable are applied here, on the CI runner, right after the
submodules are checked out. Applying the patch twice is a no-op.

Changes:
  1. Archive root auto path ("archive>") activates EVERY member instead of
     only the first level below the prefix, so deeply nested files such as
     system/movie/*.mp4 become resolvable.
  2. When GetFirstIndexStartsWith (a binary search that requires a sorted
     index) returns -1, fall back to a linear scan: third-party xp3 packers
     may leave the index unsorted.
  3. For the archive root entry the placed path keeps the full member name
     (the table key remains the basename); otherwise "archive>basename"
     paths that do not exist in the archive index would be produced.
  4. Trace archive member opens (TVPCreateStream archive branch) so xp3
     video/font failures are visible in engine.log.
  5. Trace prerendered font mapping (storage + placed path + ctor failure).
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KRKRZ = REPO / "external" / "krkrz"

BLOCK1_OLD = """\t\t\t\t// get first index which the item has 'in_arc_name' as its start
\t\t\t\t// of the string.
\t\t\t\ttjs_int i = arc->GetFirstIndexStartsWith(in_arc_name);
\t\t\t\tif(i != -1)
\t\t\t\t{
\t\t\t\t\tfor(; i < (tjs_int)storagecount; i++)
\t\t\t\t\t{
\t\t\t\t\t\tttstr name = arc->GetName(i);

\t\t\t\t\t\tif(name.StartsWith(in_arc_name))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tif(!TJS_strchr(name.c_str() + in_arc_name_len, TJS_W('/')))
\t\t\t\t\t\t\t{
"""

BLOCK1_NEW = """\t\t\t\t// get first index which the item has 'in_arc_name' as its start
\t\t\t\t// of the string.
\t\t\t\ttjs_int i = arc->GetFirstIndexStartsWith(in_arc_name);
#if defined(__OHOS__)
\t\t\t\tif(i == -1)
\t\t\t\t{
\t\t\t\t\t/* The archive index may not be sorted (third-party xp3
\t\t\t\t\t * packers): GetFirstIndexStartsWith is a binary search
\t\t\t\t\t * that requires a sorted index. Fall back to a linear
\t\t\t\t\t * scan so auto paths still resolve. */
\t\t\t\t\tfor(i = 0; i < (tjs_int)storagecount; i++)
\t\t\t\t\t{
\t\t\t\t\t\tif(arc->GetName(i).StartsWith(in_arc_name)) break;
\t\t\t\t\t}
\t\t\t\t\tif(i >= (tjs_int)storagecount) i = -1;
\t\t\t\t}
#endif
\t\t\t\tif(i != -1)
\t\t\t\t{
\t\t\t\t\tfor(; i < (tjs_int)storagecount; i++)
\t\t\t\t\t{
\t\t\t\t\t\tttstr name = arc->GetName(i);

\t\t\t\t\t\tif(name.StartsWith(in_arc_name))
\t\t\t\t\t\t{
#if defined(__OHOS__)
\t\t\t\t\t\t\t/* The archive root auto path ("archive>") must
\t\t\t\t\t\t\t * activate EVERY member, including deeply nested
\t\t\t\t\t\t\t * files (system/movie/*.mp4): the original check
\t\t\t\t\t\t\t * only activates the first level below the
\t\t\t\t\t\t\t * registered prefix. */
\t\t\t\t\t\t\tif(in_arc_name_len == 0 ||
\t\t\t\t\t\t\t\t!TJS_strchr(name.c_str() + in_arc_name_len, TJS_W('/')))
#else
\t\t\t\t\t\t\tif(!TJS_strchr(name.c_str() + in_arc_name_len, TJS_W('/')))
#endif
\t\t\t\t\t\t\t{
"""

BLOCK2_OLD = """\t\t\t\t\t\t\t{
\t\t\t\t\t\t\t\tttstr actualname = TVPExtractStorageName(name);
\t\t\t\t\t\t\t\tttstr sname = actualname;
\t\t\t\t\t\t\t\tsname.ToLowerCase();
\t\t\t\t\t\t\t\t// TODO アーカイブの時もプロパティ情報追加
\t\t\t\t\t\t\t\tTVPAutoPathTable.Add(sname, tTVPFileInfo(path, actualname) );
\t\t\t\t\t\t\t\tcount ++;
\t\t\t\t\t\t\t}
"""

BLOCK2_NEW = """\t\t\t\t\t\t\t{
#if defined(__OHOS__)
\t\t\t\t\t\t\t\t/* The table key stays the basename, but the
\t\t\t\t\t\t\t\t * placed path must carry the member's directory
\t\t\t\t\t\t\t\t * inside the archive, otherwise the archive root
\t\t\t\t\t\t\t\t * entry would produce "archive>basename" paths
\t\t\t\t\t\t\t\t * that do not exist in the index. */
\t\t\t\t\t\t\t\tttstr actualname = (in_arc_name_len == 0)
\t\t\t\t\t\t\t\t\t? name : TVPExtractStorageName(name);
#else
\t\t\t\t\t\t\t\tttstr actualname = TVPExtractStorageName(name);
#endif
\t\t\t\t\t\t\t\tttstr sname = TVPExtractStorageName(name);
\t\t\t\t\t\t\t\tsname.ToLowerCase();
\t\t\t\t\t\t\t\t// TODO アーカイブの時もプロパティ情報追加
\t\t\t\t\t\t\t\tTVPAutoPathTable.Add(sname, tTVPFileInfo(path, actualname) );
\t\t\t\t\t\t\t\tcount ++;
\t\t\t\t\t\t\t}
"""

BLOCK3_OLD = """\t\tarc = TVPArchiveCache.Get(arcname);
\t\ttry
\t\t{
\t\t\tttstr in_arc_name(sharp_pos + 1);
\t\t\ttTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
\t\t\tstream = arc->CreateStream(in_arc_name);
\t\t}
\t\tcatch(...)
\t\t{
\t\t\tarc->Release();
\t\t\tif(access >= 1) TVPClearStorageCaches();
\t\t\tthrow;
\t\t}
"""

BLOCK3_NEW = """\t\tarc = TVPArchiveCache.Get(arcname);
\t\ttry
\t\t{
\t\t\tttstr in_arc_name(sharp_pos + 1);
\t\t\ttTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
#if defined(__OHOS__)
\t\t\tTVPAddLog(ttstr(TJS_W("(info) OHOS archive open: member=")) +
\t\t\t\tin_arc_name + TJS_W(" arc=") + arcname);
#endif
\t\t\tstream = arc->CreateStream(in_arc_name);
#if defined(__OHOS__)
\t\t\tTVPAddLog(ttstr(TJS_W("(info) OHOS archive open: ok member=")) +
\t\t\t\tin_arc_name);
#endif
\t\t}
\t\tcatch(...)
\t\t{
#if defined(__OHOS__)
\t\t\tTVPAddLog(TJS_W("(info) OHOS archive open: FAILED"));
#endif
\t\t\tarc->Release();
\t\t\tif(access >= 1) TVPClearStorageCaches();
\t\t\tthrow;
\t\t}
"""

BLOCK4_OLD = """void TVPMapPrerenderedFont(const tTVPFont & font, const ttstr & storage)
{
\t// map specified font to specified prerendered font
\tttstr fn = TVPSearchPlacedPath(storage);
"""

BLOCK4_NEW = """void TVPMapPrerenderedFont(const tTVPFont & font, const ttstr & storage)
{
\t// map specified font to specified prerendered font
#if defined(__OHOS__)
\tTVPAddLog(ttstr(TJS_W("(info) OHOS font map: storage=")) + storage);
#endif
\tttstr fn = TVPSearchPlacedPath(storage);
#if defined(__OHOS__)
\tTVPAddLog(ttstr(TJS_W("(info) OHOS font map: placed=")) + fn);
#endif
"""

BLOCK5_OLD = """\t} catch(...) {
\t\tif( stream ) delete stream;
\t\tif( Image ) delete[] Image;
\t\tthrow;
\t}
"""

BLOCK5_NEW = """\t} catch(...) {
#if defined(__OHOS__)
\t\tTVPAddLog(ttstr(TJS_W("(info) OHOS prerendered font ctor FAILED: ")) + storage);
#endif
\t\tif( stream ) delete stream;
\t\tif( Image ) delete[] Image;
\t\tthrow;
\t}
"""

BLOCK6_OLD = """#include "PrerenderedFont.h"
#include "BinaryStream.h"
#include "MsgIntf.h"
"""

BLOCK6_NEW = """#include "PrerenderedFont.h"
#include "BinaryStream.h"
#include "MsgIntf.h"
#include "DebugIntf.h" /* TVPAddLog for OHOS diagnostics */
"""

BLOCKS = [
    (
        KRKRZ / "base" / "StorageIntf.cpp",
        "root-activates-all", BLOCK1_OLD, BLOCK1_NEW,
        "in_arc_name_len == 0 ||",
    ),
    (
        KRKRZ / "base" / "StorageIntf.cpp",
        "full-member-name", BLOCK2_OLD, BLOCK2_NEW,
        "(in_arc_name_len == 0)",
    ),
    (
        KRKRZ / "base" / "StorageIntf.cpp",
        "archive-open-trace", BLOCK3_OLD, BLOCK3_NEW,
        "OHOS archive open: member=",
    ),
    (
        KRKRZ / "visual" / "LayerBitmapImpl.cpp",
        "font-map-trace", BLOCK4_OLD, BLOCK4_NEW,
        "OHOS font map: storage=",
    ),
    (
        KRKRZ / "visual" / "PrerenderedFont.cpp",
        "font-ctor-trace", BLOCK5_OLD, BLOCK5_NEW,
        "OHOS prerendered font ctor FAILED:",
    ),
    (
        KRKRZ / "visual" / "PrerenderedFont.cpp",
        "font-debugintf-include", BLOCK6_OLD, BLOCK6_NEW,
        "DebugIntf.h\" /* TVPAddLog",
    ),
]


def main() -> int:
    applied = 0
    for path, name, old, new, marker in BLOCKS:
        text = path.read_text(encoding="utf-8")
        if marker in text:
            print(f"block '{name}' already applied, skipping")
            continue
        if old not in text:
            print(
                f"ERROR: pattern for block '{name}' not found in {path}",
                file=sys.stderr,
            )
            raise SystemExit(1)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        applied += 1
        print(f"applied block '{name}' to {path}")
    if applied:
        print(f"patched {applied} block(s)")
    else:
        print("nothing to do, all blocks already applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
