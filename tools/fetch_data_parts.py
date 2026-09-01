#!/usr/bin/env python3
"""Fetch the game data from the data source release.

Game data is not stored in git at all. The data location is resolved
automatically: the owner/repo comes from --repo, the GITHUB_REPOSITORY
environment variable (CI) or the origin remote, and the release tag is the
numerically highest data-vN release (drafts skipped, prereleases included).
Its release assets are
multipart zips described by data-assets.json (schemaVersion 2: name/size/
sha256 per part). This script downloads every part (skipping and reusing
files that are already complete in the download directory), verifies every
SHA-256 digest, and extracts them into the destination directory so the
release workflows and local builds find data/ populated exactly as before.
"""

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
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


def resolve_latest_data_tag(repo: str):
    """Return the highest data-vN release tag in the repo (prereleases
    included, drafts skipped), or None when nothing matches or the API
    is unreachable."""
    url = "https://api.github.com/repos/%s/releases?per_page=100" % repo
    try:
        request = urllib.request.Request(url, headers={
            "User-Agent": "yosuga-data-fetch/1.0",
            "Accept": "application/vnd.github+json",
        })
        with urllib.request.urlopen(request, timeout=60) as response:
            releases = json.load(response)
    except OSError as exc:
        log("warning: cannot list releases from %s: %s" % (url, exc))
        return None
    best, best_n = None, -1
    for release in releases:
        if release.get("draft"):
            continue
        match = re.match(r"^data-v(\d+)$", str(release.get("tag_name", "")))
        if match and int(match.group(1)) > best_n:
            best, best_n = release["tag_name"], int(match.group(1))
    return best


def repo_from_git():
    """owner/name from the origin remote, or None."""
    try:
        url = subprocess.check_output(["git", "remote", "get-url", "origin"],
                                      text=True).strip()
    except Exception:
        return None
    match = re.search(r"([^/]+)/([^/]+?)(?:\.git)?/?$", url)
    return "%s/%s" % (match.group(1), match.group(2)) if match else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="",
                        help="Release download base, e.g. "
                             "https://github.com/OWNER/REPO/releases/download/data-v1 "
                             "(default: auto-pick the highest data-vN)")
    parser.add_argument("--repo", default="",
                        help="owner/name of the data repository (default: "
                             "GITHUB_REPOSITORY, then the origin remote)")
    parser.add_argument("--dest", default="data",
                        help="Directory to extract the game data into (default: data)")
    parser.add_argument("--work", default="",
                        help="Download cache directory (default: a fresh temp dir)")
    parser.add_argument("--tag", default="",
                        help="Explicit data release tag (e.g. data-v2). "
                             "Default: auto-pick the highest data-vN release")
    args = parser.parse_args()

    if args.url:
        base = args.url.rstrip("/")
        if args.tag:
            base = re.sub(r"/releases/download/[^/]+",
                          "/releases/download/" + args.tag, base)
    else:
        repo = args.repo or os.environ.get("GITHUB_REPOSITORY") or repo_from_git()
        if not repo:
            raise SystemExit("error: cannot determine owner/repo; pass --repo "
                             "or --url, or set GITHUB_REPOSITORY")
        if args.tag:
            tag = args.tag
        else:
            tag = resolve_latest_data_tag(repo)
            if not tag:
                raise SystemExit("error: no data-vN release found in %s; "
                                 "publish one first or pass --tag" % repo)
        base = "https://github.com/%s/releases/download/%s" % (repo, tag)
        log("data source: %s (release %s%s)"
            % (base, tag, ", auto-picked" if not args.tag else ", forced"))
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
