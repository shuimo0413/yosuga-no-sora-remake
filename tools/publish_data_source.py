#!/usr/bin/env python3
"""Detect game-data changes and publish a new data source release.

The game data lives in a GitHub release, not in git, so CI can never see
local data/ edits. This is the single command that closes the loop:

    python tools/publish_data_source.py

1. Recompute the full content manifest and compare it with the last commit;
   exit early when nothing changed.
2. Bump the release tag (data-v1 -> data-v2 -> ...) or take --tag.
3. Package data/ into multipart zips plus data-assets.json and BUILD-INFO.txt
   (same layout as tools/package_data_release.py, parts stay under the 2 GB
   GitHub release-asset limit).
4. Upload the release with the gh CLI when available; otherwise print exact
   manual upload instructions and stop (rerun with --finalize afterwards).
5. Rewrite data-source.json (the repository-level pointer every consumer
   follows) and commit it together with the refreshed content manifest.

NOTE: CI cannot detect data/ edits by itself because the content is not in
git -- running this script after changing data/ is the automation.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
POINTER = os.path.join(REPO, "data-source.json")
MANIFEST = os.path.join(REPO, "data", "content-manifest.json")
ASSET_LIMIT = 2 * 1000 * 1000 * 1000  # GitHub release assets: 2 GB each


def run(command, **kwargs):
    subprocess.run(command, check=True, **kwargs)


def git(*args):
    return subprocess.check_output(["git"] + list(args), cwd=REPO, text=True).strip()


def derive_repo_slug(forced: str) -> str:
    if forced:
        return forced
    origin = git("remote", "get-url", "origin")
    import re
    # Covers github.com, ssh.github.com:443, git@github.com:, and https URLs.
    match = re.search(r"github\.com(?::\d+)?[:/]([^/]+/[^/]+?)(?:\.git)?/?$", origin)
    if not match:
        raise SystemExit("error: cannot derive the GitHub repo from origin (%s); "
                         "pass --repo owner/name" % origin)
    return match.group(1)


def read_json(path):
    import io
    return json.load(io.open(path, encoding="utf-8"))


def write_pointer(tag: str, out_dir: str, repo_slug: str) -> None:
    packaged = read_json(os.path.join(out_dir, "data-assets.json"))
    pointer = {
        "schemaVersion": 1,
        "releaseTag": packaged["releaseTag"],
        "baseUrl": "https://github.com/%s/releases/download/%s" % (repo_slug, tag),
        "kind": packaged["kind"],
        "totalSize": packaged["totalSize"],
        "fileCount": sum(a["fileCount"] for a in packaged["assets"]),
        "assets": [{"name": a["name"], "size": a["size"], "sha256": a["sha256"]}
                   for a in packaged["assets"]],
    }
    with open(POINTER, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(pointer, ensure_ascii=False, indent=2) + "\n")
    done = read_json(POINTER)
    print("data-source.json now points at %s -> %s"
          % (done["releaseTag"], done["baseUrl"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default="",
                        help="Next source release tag (default: auto-bump data-vN)")
    parser.add_argument("--repo", default="",
                        help="owner/name used for the upload URL "
                             "(default: the origin remote)")
    parser.add_argument("--max-part-mib", type=int, default=1800,
                        help="Max zip part size in MiB (default: 1800)")
    parser.add_argument("--out", default="",
                        help="Output directory (default: <repo>/../data-source-release-<tag>)")
    parser.add_argument("--skip-upload", action="store_true",
                        help="Only package locally; never touch gh")
    parser.add_argument("--finalize", action="store_true",
                        help="Rerun after a manual upload: point data-source.json "
                             "at the already-packaged release and commit")
    args = parser.parse_args()

    def default_out(tag: str) -> str:
        return args.out or os.path.join(os.path.dirname(REPO),
                                        "data-source-release-" + tag)

    # --finalize: the release was uploaded manually (or by a previous run);
    # point the repository pointer at it and commit.
    if args.finalize:
        tag = args.tag
        if not tag:
            raise SystemExit("error: --finalize needs --tag <the tag you uploaded>")
        out_dir = default_out(tag)
        if not os.path.exists(os.path.join(out_dir, "data-assets.json")):
            raise SystemExit("error: %s not found; pass --out if you used a "
                             "custom output directory" % os.path.join(out_dir, "data-assets.json"))
        write_pointer(tag, out_dir, derive_repo_slug(args.repo))
        run(["git", "add", "data-source.json", "data/content-manifest.json"], cwd=REPO)
        run(["git", "commit", "-m",
             "data source update: %s" % tag], cwd=REPO)
        print("committed. Push when ready: git push origin %s" % git("branch", "--show-current"))
        return 0

    # 1. Change detection: the committed content manifest is the snapshot of
    #    the last published tree; recomputing it reveals any data/ edit.
    run([sys.executable, os.path.join(TOOLS, "generate_content_manifest.py"),
         "--root", os.path.join(REPO, "data"),
         "--output", MANIFEST,
         "--config", os.path.join(REPO, "content-packs.json"),
         "--cache", os.path.join(REPO, "tools", ".content-manifest-cache.json"),
         "--full"])
    changed = subprocess.run(["git", "diff", "--quiet", "--", "data/content-manifest.json"],
                             cwd=REPO).returncode != 0
    if not changed:
        print("data/ is unchanged relative to the last commit; nothing to publish")
        return 0

    # 2. Tag bump.
    if args.tag:
        tag = args.tag
    elif os.path.exists(POINTER):
        previous = read_json(POINTER).get("releaseTag", "data-v0")
        number = int(previous.rsplit("v", 1)[-1]) + 1
        tag = "data-v%d" % number
    else:
        tag = "data-v1"
    print("publishing source release: %s" % tag)
    out_dir = default_out(tag)

    # 3. Package.
    if int(args.max_part_mib) * 1024 * 1024 > ASSET_LIMIT:
        raise SystemExit("error: --max-part-mib exceeds the 2 GB release asset limit")
    repo_slug = derive_repo_slug(args.repo)
    base_url = "https://github.com/%s/releases/download/%s/" % (repo_slug, tag)
    run([sys.executable, os.path.join(TOOLS, "package_data_release.py"),
         "--root", os.path.join(REPO, "data"),
         "--config", os.path.join(REPO, "content-packs.json"),
         "--tag", tag,
         "--base-url", base_url,
         "--out", out_dir,
         "--max-size", str(args.max_part_mib)])

    assets = read_json(os.path.join(out_dir, "data-assets.json"))["assets"]
    commit = git("rev-parse", "HEAD")
    info_lines = [
        "Yosuga no Sora: HD Remake",
        "Kind: game data source release (input for CI and local development)",
        "Release: %s" % tag,
        "Commit: %s" % commit,
        "Total size: %d bytes (%d files)" % (
            sum(a["size"] for a in assets),
            sum(a["fileCount"] for a in assets)),
        "",
        "Verify each download against its SHA-256 below; tools/fetch_data_parts.py",
        "performs download + verification + extraction.",
        "",
    ]
    info_lines += ["%s  %s" % (a["sha256"], a["name"]) for a in assets]
    with open(os.path.join(out_dir, "BUILD-INFO.txt"), "w",
              encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(info_lines) + "\n")
    print("packaged into %s (%d parts)" % (out_dir, len(assets)))

    # 4. Upload (gh when available).
    upload_paths = [os.path.join(out_dir, a["name"]) for a in assets]
    upload_paths += [os.path.join(out_dir, "data-assets.json"),
                     os.path.join(out_dir, "BUILD-INFO.txt")]
    uploaded = False
    if args.skip_upload:
        print("--skip-upload given; skipping the gh step")
    elif shutil.which("gh"):
        print("uploading via gh ...")
        result = subprocess.run(
            ["gh", "release", "create", tag, "--repo", repo_slug,
             "--prerelease", "--title", tag,
             "--notes", "Game data source release. Verify downloads with "
                        "BUILD-INFO.txt; consume with tools/fetch_data_parts.py."]
            + upload_paths,
            cwd=REPO)
        if result.returncode == 0:
            uploaded = True
        else:
            print("gh upload failed (exit %d)" % result.returncode)
    else:
        print("gh CLI not available; upload manually")
    if not uploaded:
        print("\nManual upload checklist:")
        print("  1. Create a PRERELEASE tagged %s on GitHub (pre-releases are" % tag)
        print("     skipped by /releases/latest/, so player downloads stay clean)")
        print("  2. Upload every file in: %s" % out_dir)
        print("  3. Rerun: python tools/publish_data_source.py --tag %s --finalize" % tag)
        return 2

    # 5. Point the repository at the new release and commit.
    write_pointer(tag, out_dir, repo_slug)
    run(["git", "add", "data-source.json", "data/content-manifest.json"], cwd=REPO)
    run(["git", "commit", "-m",
         "data source update: %s (%d parts, %d files)"
         % (tag, len(assets), sum(a["fileCount"] for a in assets))], cwd=REPO)
    print("committed. Push when ready: git push origin %s" % git("branch", "--show-current"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
