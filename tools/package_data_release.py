#!/usr/bin/env python3
"""Package the game data/ tree into downloadable zip archives.

The OpenHarmony HAP does not bundle the multi-GiB game data. The CI workflow
publishes the data instead as a set of INDEPENDENT zip archives (one per
batch of content packs, each below the 2 GiB GitHub limit). Every archive is
a plain zip32 file that the in-game downloader/import flow can extract on
the device without merging volumes or zip64 support.

Every archive stores its entries under the "data/" prefix, so extracting all
archives into the application data folder reproduces the original tree:

    <app folder>/data/startup.tjs
    <app folder>/data/system/...

data-assets.json (shipped inside the HAP) lists the archives, their sizes and
SHA-256 hashes so the app can download them in order and verify integrity.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import sys
import zipfile
from typing import Any, Dict, Iterable, List, Tuple


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Content root (the data/ directory)")
    parser.add_argument("--config", type=Path, required=True, help="content-packs.json")
    parser.add_argument("--tag", required=True, help="Release tag, e.g. v0.1.0-test.1")
    parser.add_argument("--out", type=Path, required=True, help="Directory for the zip assets")
    parser.add_argument("--max-size", type=int, default=1800, help="Max raw size per zip in MiB")
    parser.add_argument("--compress-level", type=int, default=6, choices=range(0, 10),
                        help="zip deflate level (0 = store)")
    return parser.parse_args()


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def iter_content_files(root: Path) -> Iterable[str]:
    for directory, directory_names, file_names in os.walk(str(root), followlinks=False):
        directory_names.sort()
        file_names.sort()
        for file_name in file_names:
            relative = os.path.relpath(os.path.join(directory, file_name), str(root))
            yield relative.replace(os.sep, "/")


def select_pack(relative: str, packs: List[Dict[str, Any]], default_pack: str,
                exclude: List[str]) -> str:
    if any(fnmatch.fnmatchcase(relative, pattern) for pattern in exclude):
        return ""
    for pack in packs:
        if any(fnmatch.fnmatchcase(relative, pattern) for pattern in pack.get("include", [])):
            return str(pack["id"])
    return str(default_pack)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    args = parse_args()
    root: Path = args.root
    if not (root / "startup.tjs").is_file():
        print("error: %s/startup.tjs is missing" % root, file=sys.stderr)
        return 1

    config = read_json(args.config)
    packs = config.get("packs", [])
    exclude = config.get("exclude", [])
    default_pack = config.get("defaultPack", "misc")

    by_pack: Dict[str, List[str]] = {}
    total = 0
    for relative in iter_content_files(root):
        pack_id = select_pack(relative, packs, default_pack, exclude)
        if not pack_id:
            continue
        try:
            size = (root / relative).stat().st_size
        except OSError:
            continue
        by_pack.setdefault(pack_id, []).append(relative)
        total += size
    print("total data size: %d bytes across %d packs" % (total, len(by_pack)))

    # Explicit packs in configured order, default pack last.
    ordered: List[Tuple[str, List[str]]] = []
    for pack in packs:
        files = by_pack.pop(pack["id"], None)
        if files:
            ordered.append((pack["id"], files))
    misc_files = by_pack.pop(default_pack, None)
    if misc_files:
        ordered.append((default_pack, misc_files))

    # Batch packs into archives below the size limit.
    max_bytes = args.max_size * 1024 * 1024
    batches: List[List[Tuple[str, List[str]]]] = []
    current: List[Tuple[str, List[str]]] = []
    current_size = 0
    for pack_id, files in ordered:
        pack_size = sum((root / f).stat().st_size for f in files)
        if current and current_size + pack_size > max_bytes:
            batches.append(current)
            current = []
            current_size = 0
        current.append((pack_id, files))
        current_size += pack_size
    if current:
        batches.append(current)

    args.out.mkdir(parents=True, exist_ok=True)
    compression = zipfile.ZIP_DEFLATED if args.compress_level > 0 else zipfile.ZIP_STORED
    assets = []
    for index, batch in enumerate(batches, start=1):
        name = "Yosuga-no-Sora-HD-Remake-OpenHarmony-data-%02d-%s.zip" % (index, args.tag)
        archive = args.out / name
        print("archiving %s ..." % name)
        file_count = 0
        with zipfile.ZipFile(str(archive), "w", compression, compresslevel=args.compress_level) as zf:
            for pack_id, files in batch:
                for relative in files:
                    zf.write(str(root / relative), "data/" + relative)
                    file_count += 1
        size = archive.stat().st_size
        assets.append({
            "name": name,
            "size": size,
            "sha256": sha256_file(archive),
            "packs": [pack_id for pack_id, _ in batch],
            "fileCount": file_count,
        })
        print("asset %s: %d bytes, packs=%s" % (name, size, ",".join(assets[-1]["packs"])))

    manifest = {
        "schemaVersion": 2,
        "releaseTag": args.tag,
        "baseUrl": "https://github.com/WarSkyGod/yosuga-no-sora-remake/releases/download/%s" % args.tag,
        "kind": "zip-parts",
        "assets": assets,
        "totalSize": total,
    }
    manifest_path = args.out / "data-assets.json"
    with manifest_path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print("manifest: %s" % manifest_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
