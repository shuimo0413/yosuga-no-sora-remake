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
import re
import subprocess
import sys
import zipfile
from typing import Any, Dict, Iterable, List, Tuple


def infer_base_url_from_git() -> str:
    """Derive the GitHub releases download root from the origin remote.

    The manifest's baseUrl must point at the repository that publishes the
    assets, which is a property of the build environment, not of the code:
    a hard-coded owner would silently redirect every fork's manifest to that
    fork. Falls back to an empty string when no GitHub origin is set; the
    caller must then fail loudly instead of guessing.
    """
    try:
        url = subprocess.run(
            ["git", "config", "--get", "remote.origin.url"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except Exception:
        return ""
    match = re.search(r"github\.com[:/]([^/]+)/(.+?)(?:\.git)?/?$", url)
    if match:
        return "https://github.com/%s/%s/releases/download" % (
            match.group(1), match.group(2))
    return ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Content root (the data/ directory)")
    parser.add_argument("--config", type=Path, required=True, help="content-packs.json")
    parser.add_argument("--tag", required=True, help="Release tag, e.g. v0.1.0-test.1")
    parser.add_argument("--base-url", default="",
                        help="Download root written into data-assets.json, e.g. "
                             "https://github.com/OWNER/REPO/releases/download. "
                             "Defaults to the origin remote when it is a GitHub URL; "
                             "the script fails when neither is available so no "
                             "hard-coded fork ever ships in the manifest.")
    parser.add_argument("--out", type=Path, required=True, help="Directory for the zip assets")
    parser.add_argument("--max-size", type=int, default=1800, help="Max raw size per zip in MiB")
    parser.add_argument("--compress-level", type=int, default=0, choices=range(0, 10),
                        help="zip deflate level (0 = store, default)")
    parser.add_argument("--manifest-only", action="store_true",
                        help="compute the batch layout and write data-assets.json without archiving "
                             "(used by the HAP build job, which runs parallel to the packaging job); "
                             "sha256 is left empty and sizes are the raw batch sizes")
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

    base_url = args.base_url or infer_base_url_from_git()
    if not base_url:
        print("error: no --base-url given and the origin remote is not a "
              "GitHub URL; refusing to write a manifest with a guessed "
              "download root", file=sys.stderr)
        return 1
    # Stored WITHOUT a trailing slash: the clients append "/" when joining
    # asset names (see Index.ets loadManifest / BootstrapActivity).
    base_url = base_url.rstrip("/")

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
        name = "Yosuga-no-Sora-HD-Remake-data-%02d-%s.zip" % (index, args.tag)
        archive = args.out / name
        file_count = 0
        for pack_id, files in batch:
            file_count += len(files)
        if args.manifest_only:
            # Parallel HAP job: just report the layout; sizes are the raw
            # batch sizes (exact for store) and sha256 is deferred to the
            # packaging job that actually writes the archives.
            size = sum((root / f).stat().st_size for pack_id, files in batch for f in files)
            raw_size = size
            sha256 = ""
            print("manifest-only %s: raw %d bytes, packs=%s" % (name, size, ",".join(p for p, _ in batch)))
        else:
            print("archiving %s ..." % name)
            with zipfile.ZipFile(str(archive), "w", compression, compresslevel=args.compress_level) as zf:
                for pack_id, files in batch:
                    for relative in files:
                        zf.write(str(root / relative), "data/" + relative)
            raw_size = sum((root / f).stat().st_size for pack_id, files in batch for f in files)
            size = archive.stat().st_size
            sha256 = sha256_file(archive)
            print("asset %s: %d bytes (raw %d), packs=%s" % (name, size, raw_size, ",".join(p for p, _ in batch)))
        assets.append({
            "name": name,
            "size": size,
            "sha256": sha256,
            "rawSize": raw_size,
            "packs": [pack_id for pack_id, _ in batch],
            "fileCount": file_count,
        })

    manifest = {
        "schemaVersion": 2,
        "releaseTag": args.tag,
        "baseUrl": base_url,
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
