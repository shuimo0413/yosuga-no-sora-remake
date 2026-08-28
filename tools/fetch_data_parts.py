#!/usr/bin/env python3
"""Fetch the game data from the data source release.

The repository checkout only carries LFS pointer stubs for data/; the real
content lives in a GitHub release whose assets are multipart zips described
by data-assets.json (schemaVersion 2: name/size/sha256 per part). This
script downloads every part (skipping and reusing files that are already
complete in the download directory), verifies every SHA-256 digest, and
extracts them into the destination directory so the release workflows can
proceed exactly as they did with an LFS checkout.
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import time
import urllib.request
import zipfile

CHUNK = 1 << 20


def log(message: str) -> None:
    print(message, flush=True)


def fetch(url: str, dest: str, attempts: int = 3) -> None:
    last_error = None
    for attempt in range(1, attempts + 1):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "yosuga-data-fetch/1.0"})
            with urllib.request.urlopen(request, timeout=120) as response, \
                    open(dest, "wb") as handle:
                while True:
                    chunk = response.read(CHUNK)
                    if not chunk:
                        break
                    handle.write(chunk)
            return
        except OSError as exc:
            last_error = exc
            log("download failed (attempt %d/%d): %s" % (attempt, attempts, exc))
            time.sleep(5 * attempt)
    raise SystemExit("error: cannot download %s: %s" % (url, last_error))


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def extract_part(archive_path: str, dest: str) -> None:
    """Extract one part zip into dest, stripping the leading 'data/'
    component that package_data_release.py prepends to every entry.
    Otherwise --dest data would nest everything under data/data/."""
    dest_abs = os.path.abspath(dest)
    with zipfile.ZipFile(archive_path) as archive:
        for member in archive.infolist():
            name = member.filename
            if name == "data/" or name == "data":
                continue
            if name.startswith("data/"):
                name = name[len("data/"):]
            if not name:
                continue
            target = os.path.abspath(os.path.join(dest, name))
            if os.path.commonpath([dest_abs, target]) != dest_abs:
                raise SystemExit("error: unsafe path in archive: %s" % member.filename)
            if name.endswith("/"):
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with archive.open(member) as source, open(target, "wb") as handle:
                shutil.copyfileobj(source, handle)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="",
                        help="Release download base, e.g. "
                             "https://github.com/OWNER/REPO/releases/download/data-v1 "
                             "(default: read baseUrl from data-source.json)")
    parser.add_argument("--source-manifest", default="data-source.json",
                        help="Repository-level pointer to the current data "
                             "source release (default: data-source.json)")
    parser.add_argument("--dest", default="data",
                        help="Directory to extract the game data into (default: data)")
    parser.add_argument("--work", default="",
                        help="Download cache directory (default: a fresh temp dir)")
    args = parser.parse_args()

    if args.url:
        base = args.url.rstrip("/")
    else:
        if not os.path.exists(args.source_manifest):
            raise SystemExit(
                "error: %s not found and no --url given; the data source is "
                "published via a GitHub release and recorded in that file"
                % args.source_manifest)
        pointer = json.load(io.open(args.source_manifest, encoding="utf-8"))
        base = str(pointer.get("baseUrl", "")).rstrip("/")
        if not base:
            raise SystemExit("error: %s has no baseUrl" % args.source_manifest)
        log("data source: %s (release %s)"
            % (base, pointer.get("releaseTag", "unknown")))
    work = args.work or tempfile.mkdtemp(prefix="data-parts-")
    os.makedirs(work, exist_ok=True)
    os.makedirs(args.dest, exist_ok=True)

    manifest_path = os.path.join(work, "data-assets.json")
    log("fetching data-assets.json from %s" % base)
    fetch(base + "/data-assets.json", manifest_path)
    manifest = json.load(io.open(manifest_path, encoding="utf-8"))
    assets = manifest.get("assets") or []
    if not assets:
        raise SystemExit("error: data-assets.json lists no assets")

    for asset in assets:
        name = asset["name"]
        part = os.path.join(work, name)
        size = asset["size"]
        if os.path.exists(part) and os.path.getsize(part) == size:
            log("reusing %s (%d bytes)" % (name, size))
        else:
            log("downloading %s (%d bytes)" % (name, size))
            fetch(base + "/" + name, part)
        if os.path.getsize(part) != size:
            raise SystemExit("error: size mismatch for %s" % name)
        digest = sha256_file(part)
        if digest != asset["sha256"]:
            raise SystemExit("error: sha256 mismatch for %s (got %s)" % (name, digest))
        log("verified %s" % name)

    for asset in assets:
        extract_part(os.path.join(work, asset["name"]), args.dest)
        log("extracted %s -> %s" % (asset["name"], args.dest))

    log("game data ready in %s (%d parts)" % (args.dest, len(assets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
