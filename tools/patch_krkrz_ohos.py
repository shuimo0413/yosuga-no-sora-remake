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
"""

import sys
from pathlib import Path

TARGET = (
    Path(__file__).resolve().parent.parent
    / "external" / "krkrz" / "base" / "StorageIntf.cpp"
)

OLD = """\t\t\t\t// get first index which the item has 'in_arc_name' as its start
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

NEW = """\t\t\t\t// get first index which the item has 'in_arc_name' as its start
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


def main() -> int:
    text = TARGET.read_text(encoding="utf-8")
    if "in_arc_name_len == 0 ||" in text:
        print("patch already applied, skipping")
        return 0
    if OLD not in text:
        print(
            "ERROR: target pattern not found in " + str(TARGET),
            file=sys.stderr,
        )
        return 1
    text = text.replace(OLD, NEW, 1)
    TARGET.write_text(text, encoding="utf-8")
    print("patched " + str(TARGET))
    return 0


if __name__ == "__main__":
    sys.exit(main())
