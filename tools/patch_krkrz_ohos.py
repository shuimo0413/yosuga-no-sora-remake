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
"""

import sys
from pathlib import Path

TARGET = (
    Path(__file__).resolve().parent.parent
    / "external" / "krkrz" / "base" / "StorageIntf.cpp"
)

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


def apply_block(text: str, name: str, old: str, new: str, marker: str):
    if marker in text:
        print(f"block '{name}' already applied, skipping")
        return text, False
    if old not in text:
        print(
            f"ERROR: pattern for block '{name}' not found in {TARGET}",
            file=sys.stderr,
        )
        raise SystemExit(1)
    return text.replace(old, new, 1), True


def main() -> int:
    text = TARGET.read_text(encoding="utf-8")
    applied = 0
    text, changed = apply_block(
        text, "root-activates-all",
        BLOCK1_OLD, BLOCK1_NEW, "in_arc_name_len == 0 ||",
    )
    applied += 1 if changed else 0
    text, changed = apply_block(
        text, "full-member-name",
        BLOCK2_OLD, BLOCK2_NEW, "(in_arc_name_len == 0)",
    )
    applied += 1 if changed else 0
    if applied:
        TARGET.write_text(text, encoding="utf-8")
        print(f"patched {TARGET} ({applied} block(s))")
    else:
        print("nothing to do, all blocks already applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
