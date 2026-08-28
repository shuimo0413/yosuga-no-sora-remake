#!/usr/bin/env python3
"""Collect third-party license texts into one THIRD-PARTY-NOTICES.txt.

Every binary release must ship the license text of every component that is
redistributed in it (krkrz's BSD-style license even requires the statement
in accompanying documentation). The release workflows call this script and
put the output next to the published assets.

Usage:
    python tools/generate_notices.py --output THIRD-PARTY-NOTICES.txt

The list below is checked against the actual submodule checkouts; missing
files are reported on stderr and the script exits non-zero so CI cannot
silently ship an incomplete notice file.
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

# (component, source path, license note shown in the header table)
COMPONENTS = [
    ("Kirikiri SDL2 (this project)", "LICENSE", "MIT"),
    ("Kirikiri Z (krkrz)", "external/krkrz/LICENSE", "BSD-style (Japanese), non-endorsement clause"),
    ("Simple DirectMedia Layer (SDL)", "external/SDL/LICENSE.txt", "zlib license"),
    ("zlib", "external/zlib/README", "zlib license (stated inside the README)"),
    ("FAudio", "external/FAudio/LICENSE", "MS-PL"),
    ("simde (SIMD-everywhere)", "external/simde/COPYING", "MIT"),
]

HEADER = """\
THIRD-PARTY NOTICES
===================

This package redistribuates the components listed below. Each section that
follows contains the full license text as shipped in the source repository.

The game content (scenario scripts, images, audio and video under data/) is
derived from the original commercial work and remains the property of its
rightful copyright holders; it is distributed here for use with this
unofficial HD remake build only.

"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, help="output file path")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    chunks = [HEADER]
    missing = []
    for name, rel, note in COMPONENTS:
        src = root / rel
        if not src.is_file():
            missing.append(rel)
            continue
        body = io.open(src, "r", encoding="utf-8", errors="replace").read().rstrip()
        chunks.append("=" * 78 + "\n")
        chunks.append("%s\nSource: %s\nLicense: %s\n\n" % (name, rel, note))
        chunks.append(body + "\n\n")
    if missing:
        print(
            "generate_notices: missing license files (run after "
            "'git submodule update --init'): %s" % ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    io.open(tmp, "w", encoding="utf-8", newline="\n").write("".join(chunks))
    tmp.replace(out)
    print("generate_notices: wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
