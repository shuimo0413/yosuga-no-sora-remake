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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True,
                        help="Release download base, e.g. "
                             "https://github.com/OWNER/REPO/releases/download/data-v1")
    parser.add_argument("--dest", default="data",
                        help="Directory to extract the game data into (default: data)")
    parser.add_argument("--work", default="",
                        help="Download cache directory (default: a fresh temp dir)")
    args = parser.parse_args()

    base = args.url.rstrip("/")
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
        with zipfile.ZipFile(os.path.join(work, asset["name"])) as archive:
            archive.extractall(args.dest)
        log("extracted %s -> %s" % (asset["name"], args.dest))

    log("game data ready in %s (%d parts)" % (args.dest, len(assets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
