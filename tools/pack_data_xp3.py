#!/usr/bin/env python3
"""Pack a directory tree into an uncompressed, unencrypted XP3 archive.

The produced archive uses the krkrz XP3 container format (version 2) with:
  * stored (uncompressed) file segments,
  * a raw (uncompressed) index,
  * no file or index encryption,
  * original directory structure preserved through relative paths.

The index entry layout mirrors base/XP3Archive.cpp in the krkrz engine:

  header   : 11-byte signature + uint64 index_offset
  file data: one segment per input file, appended in order
  index    : 1-byte index flag (0 = raw) + uint64 index size + chunks
             chunk = 4-byte tag + uint64 content size + content
             each file -> "File" chunk containing:
               "info" : uint32 flags + uint64 org_size + uint64 arc_size
                        + uint16 name_len + UTF-16LE name
               "segm" : uint32 flags + uint64 offset + uint64 org_size
                        + uint64 arc_size   (28 bytes per segment)
               "adlr" : uint32 adler32 of the original data
"""

import argparse
import os
import struct
import zlib

# "XP3" + CR LF + " " + LF + 0x1a + 0x8b + 0x67 + 0x01
SIGNATURE = bytes([0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01])


def make_chunk(tag: bytes, content: bytes) -> bytes:
    return tag + struct.pack("<Q", len(content)) + content


def pack_directory(root: str, output: str) -> int:
    entries = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for filename in filenames:
            full = os.path.join(dirpath, filename)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as stream:
                data = stream.read()
            entries.append((rel, data))
    entries.sort(key=lambda entry: entry[0])

    with open(output, "wb") as out:
        out.write(SIGNATURE)
        out.write(struct.pack("<Q", 0))  # index offset placeholder

        data_offset = out.tell()
        index_entries = []
        for rel, data in entries:
            index_entries.append((rel, data, data_offset))
            out.write(data)
            data_offset += len(data)

        index_data = bytearray()
        for rel, data, offset in index_entries:
            name = rel.encode("utf-16-le")
            info_content = (
                struct.pack("<IQQ", 0, len(data), len(data))
                + struct.pack("<H", len(rel))
                + name
            )
            segm_content = struct.pack("<IQQQ", 0, offset, len(data), len(data))
            adlr_content = struct.pack("<I", zlib.adler32(data) & 0xFFFFFFFF)
            file_content = (
                make_chunk(b"info", info_content)
                + make_chunk(b"segm", segm_content)
                + make_chunk(b"adlr", adlr_content)
            )
            index_data += make_chunk(b"File", file_content)

        index_offset = data_offset
        out.seek(11)
        out.write(struct.pack("<Q", index_offset))
        out.seek(index_offset)
        out.write(b"\x00")  # index flag: raw (uncompressed), no continuation
        out.write(struct.pack("<Q", len(index_data)))
        out.write(index_data)

    return len(entries)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, help="Directory tree to pack")
    parser.add_argument("--output", required=True, help="Output .xp3 path")
    args = parser.parse_args()

    count = pack_directory(args.root, args.output)
    print(f"packed {count} files into {args.output}")


if __name__ == "__main__":
    main()
