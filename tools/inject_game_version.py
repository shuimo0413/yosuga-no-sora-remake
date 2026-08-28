#!/usr/bin/env python3
"""Inject the game version from CMakeLists.txt into data/system/status.tjs.

The single version source is the KRKRSDL2_VERSION_NAME cache entry in the
root CMakeLists.txt (MACOSX_BUNDLE_*_VERSION_STRING and friends consume the
same value at configure time). The release workflows call this right before
packing the data directory so every published package carries a status.tjs
whose GAME_VERSION matches the build; the checked-in file is only the
day-to-day development fallback and is overwritten, never hand-synced.
"""

import argparse
import re
import sys

CMAKE_PATTERN = re.compile(
    r'^\s*set\(KRKRSDL2_VERSION_NAME\s+"([^"]+)"', re.MULTILINE)
STATUS_PATTERN = re.compile(rb'var GAME_VERSION = "[^"]*";')


def read_version(cmake_path: str) -> str:
    try:
        with open(cmake_path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        print("error: cannot read %s: %s" % (cmake_path, exc), file=sys.stderr)
        raise SystemExit(1)
    match = CMAKE_PATTERN.search(text)
    if not match:
        print("error: KRKRSDL2_VERSION_NAME not found in %s" % cmake_path,
              file=sys.stderr)
        raise SystemExit(1)
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="CMakeLists.txt",
                        help="Root CMakeLists.txt (version source)")
    parser.add_argument("--status", default="data/system/status.tjs",
                        help="status.tjs to rewrite in place")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the resolved version without writing")
    args = parser.parse_args()

    version = read_version(args.cmake)
    if args.dry_run:
        print("resolved GAME_VERSION: %s (no write)" % version)
        return 0

    try:
        with open(args.status, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        print("error: cannot read %s: %s" % (args.status, exc), file=sys.stderr)
        return 1
    replacement = b'var GAME_VERSION = "%s";' % version.encode("utf-8")
    new_data, count = STATUS_PATTERN.subn(replacement, data, count=1)
    if count != 1:
        print("error: GAME_VERSION assignment not found in %s" % args.status,
              file=sys.stderr)
        return 1
    # Byte-level rewrite keeps the original line endings intact.
    with open(args.status, "wb") as handle:
        handle.write(new_data)
    print("status.tjs GAME_VERSION -> %s" % version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
