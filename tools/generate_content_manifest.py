#!/usr/bin/env python3
"""Generate a deterministic, incrementally hashed content manifest."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unicodedata
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Sequence, Tuple


CACHE_SCHEMA_VERSION = 1
MANIFEST_SCHEMA_VERSION = 1
WINDOWS_RESERVED_NAMES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
}
WINDOWS_INVALID_CHARS = set('<>:"\\|?*')


class ManifestError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Content root")
    parser.add_argument("--output", type=Path, required=True, help="Manifest output")
    parser.add_argument("--config", type=Path, required=True, help="Pack rules JSON")
    parser.add_argument("--cache", type=Path, required=True, help="Incremental hash cache")
    parser.add_argument(
        "--full",
        action="store_true",
        help="Ignore the cache and hash every file again",
    )
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args()


def read_json(path: Path, default: Any = None) -> Any:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError:
        if default is not None:
            return default
        raise ManifestError(f"JSON file not found: {path}")
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"Cannot read JSON file {path}: {error}") from error


def atomic_write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(str(temporary_path), str(path))
        try:
            path.chmod(0o644)
        except OSError:
            pass
    except Exception:
        try:
            temporary_path.unlink()
        except OSError:
            pass
        raise


def normalize_relative_path(path: Path, root: Path) -> str:
    relative = path.relative_to(root).as_posix()
    normalized = unicodedata.normalize("NFC", relative)
    if normalized != relative:
        raise ManifestError(f"Path is not NFC-normalized: {relative}")
    return relative


def is_excluded(path: str, patterns: Sequence[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def validate_portable_path(path: str) -> None:
    if unicodedata.normalize("NFC", path) != path:
        raise ManifestError(f"Path is not NFC-normalized: {path}")
    for part in path.split("/"):
        if not part or part in {".", ".."}:
            raise ManifestError(f"Invalid path component: {path}")
        if part.endswith((" ", ".")):
            raise ManifestError(f"Windows-incompatible trailing character: {path}")
        if any(character in WINDOWS_INVALID_CHARS for character in part):
            raise ManifestError(f"Windows-incompatible character in path: {path}")
        stem = part.split(".", 1)[0].upper()
        if stem in WINDOWS_RESERVED_NAMES:
            raise ManifestError(f"Windows-reserved path component: {path}")


def iter_content_files(root: Path, excluded: Sequence[str]) -> Iterable[Tuple[str, Path]]:
    seen_casefolded: Dict[str, str] = {}
    for directory, directory_names, file_names in os.walk(str(root), followlinks=False):
        directory_names.sort()
        file_names.sort()
        directory_path = Path(directory)
        for directory_name in directory_names:
            physical_directory = directory_path / directory_name
            relative_directory = normalize_relative_path(physical_directory, root)
            if physical_directory.is_symlink() and not is_excluded(
                relative_directory, excluded
            ):
                raise ManifestError(
                    "Symbolic links are not portable content assets: "
                    f"{relative_directory}"
                )
        for file_name in file_names:
            physical_path = directory_path / file_name
            relative_path = normalize_relative_path(physical_path, root)
            if is_excluded(relative_path, excluded):
                continue
            if physical_path.is_symlink():
                raise ManifestError(
                    f"Symbolic links are not portable content assets: {relative_path}"
                )
            if not physical_path.is_file():
                continue
            validate_portable_path(relative_path)
            collision_key = relative_path.casefold()
            previous = seen_casefolded.get(collision_key)
            if previous is not None and previous != relative_path:
                raise ManifestError(
                    f"Case-insensitive path collision: {previous} <-> {relative_path}"
                )
            seen_casefolded[collision_key] = relative_path
            yield relative_path, physical_path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(4 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def load_cache(path: Path, ignore: bool) -> Mapping[str, Mapping[str, Any]]:
    if ignore:
        return {}
    cache = read_json(path, default={})
    if not isinstance(cache, dict) or cache.get("schemaVersion") != CACHE_SCHEMA_VERSION:
        return {}
    entries = cache.get("files", {})
    return entries if isinstance(entries, dict) else {}


def select_pack(path: str, packs: Sequence[Mapping[str, Any]], default_pack: str) -> str:
    for pack in packs:
        patterns = pack.get("include", [])
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns):
            return str(pack["id"])
    return default_pack


def digest_entries(entries: Sequence[Mapping[str, Any]]) -> str:
    digest = hashlib.sha256()
    for entry in entries:
        digest.update(str(entry["path"]).encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(entry["size"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(str(entry["sha256"]).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def validate_config(config: Mapping[str, Any]) -> Tuple[List[Mapping[str, Any]], str, List[str]]:
    if config.get("schemaVersion") != MANIFEST_SCHEMA_VERSION:
        raise ManifestError(
            f"Unsupported pack config schemaVersion: {config.get('schemaVersion')}"
        )
    packs = config.get("packs")
    default_pack = config.get("defaultPack")
    excluded = config.get("exclude", [])
    if not isinstance(packs, list) or not packs:
        raise ManifestError("Config must contain a non-empty 'packs' array")
    if not isinstance(default_pack, str) or not default_pack:
        raise ManifestError("Config must contain 'defaultPack'")
    if not isinstance(excluded, list) or not all(isinstance(item, str) for item in excluded):
        raise ManifestError("Config 'exclude' must be an array of strings")
    identifiers = []
    for pack in packs:
        if not isinstance(pack, dict) or not isinstance(pack.get("id"), str):
            raise ManifestError("Each pack must contain a string 'id'")
        if not isinstance(pack.get("include"), list):
            raise ManifestError(f"Pack {pack['id']} must contain an 'include' array")
        identifiers.append(pack["id"])
    identifiers.append(default_pack)
    if len(identifiers) != len(set(identifiers)):
        raise ManifestError("Pack ids and defaultPack must be unique")
    return packs, default_pack, excluded


def generate(args: argparse.Namespace) -> Mapping[str, Any]:
    root = args.root.resolve()
    if not root.is_dir():
        raise ManifestError(f"Content root is not a directory: {root}")

    config = read_json(args.config.resolve())
    if not isinstance(config, dict):
        raise ManifestError("Pack config must be a JSON object")
    packs, default_pack, excluded = validate_config(config)
    output = args.output.resolve()
    try:
        output_relative = normalize_relative_path(output, root)
    except ValueError:
        output_relative = ""
    if output_relative:
        excluded = [*excluded, output_relative]

    cached_files = load_cache(args.cache.resolve(), args.full)
    next_cache: MutableMapping[str, Mapping[str, Any]] = {}
    manifest_files: List[Mapping[str, Any]] = []
    reused_hashes = 0
    calculated_hashes = 0

    for relative_path, physical_path in iter_content_files(root, excluded):
        stat = physical_path.stat()
        size = stat.st_size
        mtime_ns = stat.st_mtime_ns
        cached = cached_files.get(relative_path)
        if (
            cached is not None
            and cached.get("size") == size
            and cached.get("mtimeNs") == mtime_ns
            and isinstance(cached.get("sha256"), str)
        ):
            checksum = str(cached["sha256"])
            reused_hashes += 1
        else:
            checksum = sha256_file(physical_path)
            # Re-stat after hashing: if the file changed between the size
            # snapshot and the digest, the manifest would pair one file's
            # size with another file's hash (TOCTOU). Fail loudly instead.
            post_stat = physical_path.stat()
            if (post_stat.st_size, post_stat.st_mtime_ns) != (size, mtime_ns):
                print(
                    "error: %s changed while it was being hashed" % relative_path,
                    file=sys.stderr,
                )
                return 1
            calculated_hashes += 1

        pack_id = select_pack(relative_path, packs, default_pack)
        manifest_files.append(
            {
                "path": relative_path,
                "size": size,
                "sha256": checksum,
                "pack": pack_id,
            }
        )
        next_cache[relative_path] = {
            "size": size,
            "mtimeNs": mtime_ns,
            "sha256": checksum,
        }

    manifest_files.sort(key=lambda item: str(item["path"]))
    pack_summaries = []
    all_pack_ids = [str(pack["id"]) for pack in packs] + [default_pack]
    for pack_id in all_pack_ids:
        entries = [entry for entry in manifest_files if entry["pack"] == pack_id]
        pack_summaries.append(
            {
                "id": pack_id,
                "fileCount": len(entries),
                "totalSize": sum(int(entry["size"]) for entry in entries),
                "contentVersion": digest_entries(entries),
            }
        )

    manifest = {
        "schemaVersion": MANIFEST_SCHEMA_VERSION,
        "contentVersion": digest_entries(manifest_files),
        "fileCount": len(manifest_files),
        "totalSize": sum(int(entry["size"]) for entry in manifest_files),
        "packs": pack_summaries,
        "files": manifest_files,
    }
    atomic_write_json(output, manifest)
    atomic_write_json(
        args.cache.resolve(),
        {"schemaVersion": CACHE_SCHEMA_VERSION, "files": next_cache},
    )
    return {
        "output": str(output),
        "contentVersion": manifest["contentVersion"],
        "fileCount": manifest["fileCount"],
        "totalSize": manifest["totalSize"],
        "hashed": calculated_hashes,
        "reused": reused_hashes,
    }


def main() -> int:
    args = parse_args()
    try:
        result = generate(args)
    except (ManifestError, OSError) as error:
        print(f"content-manifest: {error}", file=sys.stderr)
        return 2
    if not args.quiet:
        print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
