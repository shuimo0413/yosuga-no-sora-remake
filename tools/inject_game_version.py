#!/usr/bin/env python3
"""Stamp the numeric game version extracted from a release tag into status.tjs.

The release tag is the single version source for every platform display
version (Android/iOS/OHOS/macOS bundle versions and the in-game
data/system/status.tjs GAME_VERSION alike). Extraction keeps the leading
numeric dotted sequence verbatim and drops everything after it:

    v1.0.0 -> 1.0.0      v1.0 -> 1.0      v1.01 -> 1.01
    v123 -> 123          v1.0.0-ohos-x -> 1.0.0      v1.0.0-test.1 -> 1.0.0

Release workflows call this right before packing the data directory so every
published package carries a status.tjs whose GAME_VERSION matches the tag;
the checked-in file is only the day-to-day development fallback.
"""

import argparse
import re
import sys

TAG_PATTERN = re.compile(r"^v?([0-9]+(?:\.[0-9]+)*)")
STATUS_PATTERN = re.compile(rb'var GAME_VERSION = "[^"]*";')


def extract_numeric_version(tag: str) -> str:
    match = TAG_PATTERN.match(tag.strip())
    if not match:
        print("error: cannot derive a numeric version from tag: %s" % tag,
              file=sys.stderr)
        raise SystemExit(1)
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True,
                        help="Release tag, e.g. v1.0.0-ohos-x")
    parser.add_argument("--status", default="data/system/status.tjs",
                        help="status.tjs to rewrite in place")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the resolved version without writing")
    args = parser.parse_args()

    version = extract_numeric_version(args.tag)
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
    print("status.tjs GAME_VERSION -> %s (from tag %s)" % (version, args.tag))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
