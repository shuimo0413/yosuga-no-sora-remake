/* SPDX-License-Identifier: MIT */
/* Self-contained XP3 extractor. See ohos_xp3_extract.h for the format notes. */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "ohos_xp3_extract.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace {

struct Segment {
  uint64_t offset;   /* in-archive file offset */
  uint64_t orgSize;  /* size after decoding */
  uint64_t arcSize;  /* size inside the archive */
  uint32_t flags;    /* 0x07: 0=raw 1=zlib */
};

struct Entry {
  std::string name;       /* relative path, UTF-8, / separators */
  std::vector<Segment> segments;
};

const unsigned char kSignature[11] = {
  0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01 };

int fail(OHOSXp3ExtractResult *result, const char *fmt, const char *arg) {
  if (result) {
    snprintf(result->error, sizeof(result->error), fmt, arg ? arg : "");
    result->ok = 0;
  }
  return -1;
}

uint64_t rd64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24);
}

bool readAt(FILE *f, uint64_t pos, void *buf, size_t len) {
  if (fseeko(f, (off_t)pos, SEEK_SET) != 0) return false;
  return fread(buf, 1, len, f) == len;
}

bool read64At(FILE *f, uint64_t pos, uint64_t &out) {
  unsigned char b[8];
  if (!readAt(f, pos, b, 8)) return false;
  out = rd64(b);
  return true;
}

/* Convert UTF-16LE (BMP + surrogate pairs) to UTF-8. */
bool utf16leToUtf8(const unsigned char *src, size_t srcBytes, std::string &out) {
  out.clear();
  size_t units = srcBytes / 2;
  for (size_t i = 0; i < units; ++i) {
    uint32_t cp = (uint32_t)src[i * 2] | ((uint32_t)src[i * 2 + 1] << 8);
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units) {
      uint32_t lo = (uint32_t)src[(i + 1) * 2] |
        ((uint32_t)src[(i + 1) * 2 + 1] << 8);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out += (char)cp;
    } else if (cp < 0x800) {
      out += (char)(0xC0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }
  return true;
}

/* Reject absolute paths and ".." so a hostile archive cannot escape the
 * output directory. Backslashes are treated as separators. */
bool sanitizeName(const std::string &in, std::string &out) {
  out.clear();
  if (in.empty() || in[0] == '/') return false;
  std::string part;
  for (size_t i = 0; i <= in.size(); ++i) {
    char c = (i < in.size()) ? in[i] : '/';
    if (c == '\\') c = '/';
    if (c == '/') {
      if (part.empty() || part == ".") {
        /* skip empty and "." components */
      } else if (part == "..") {
        return false;
      } else {
        if (!out.empty()) out += "/";
        out += part;
      }
      part.clear();
    } else {
      part += c;
    }
  }
  return !out.empty();
}

bool mkdirs(const std::string &path) {
  /* path is a directory path; create every missing level */
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] == '/' || i + 1 == path.size()) {
      if (!cur.empty() && cur != "/") {
        if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
          struct stat st;
          if (stat(cur.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return false;
        }
      }
    }
  }
  return true;
}

/* Locate a chunk tagged with the given 4-byte tag inside
 * data[start, start+size). On a match sets start to the content start and
 * size to the content size; otherwise scans forward (mirrors
 * tTVPXP3Archive::FindChunk). */
bool findChunk(const unsigned char *data, uint32_t indexSize,
  const unsigned char tag[4], uint32_t &start, uint32_t &size) {
  uint32_t pos = start;
  while (pos < size) {
    if (pos + 12 > size) return false;
    bool match = memcmp(data + pos, tag, 4) == 0;
    uint64_t chunkSize = rd64(data + pos + 4);
    if (chunkSize > 0x7FFFFFFFULL) return false;
    if (match) {
      start = pos + 12;
      size = (uint32_t)chunkSize;
      return true;
    }
    pos += 12 + (uint32_t)chunkSize;
  }
  return false;
}

bool parseIndex(const unsigned char *data, uint32_t indexSize,
  std::vector<Entry> &entries, OHOSXp3ExtractResult *result) {
  const unsigned char tagFile[4] = { 0x46, 0x69, 0x6c, 0x65 };
  const unsigned char tagInfo[4] = { 0x69, 0x6e, 0x66, 0x6f };
  const unsigned char tagSegm[4] = { 0x73, 0x65, 0x67, 0x6d };
  uint32_t fileStart = 0;
  uint32_t fileSize = indexSize;
  for (;;) {
    if (!findChunk(data, indexSize, tagFile, fileStart, fileSize)) break;
    uint32_t subStart = fileStart;
    uint32_t subSize = fileSize;
    if (!findChunk(data, indexSize, tagInfo, subStart, subSize))
      return false;
    if (subSize < 22) return false;
    /* info: u32 flags, u64 orgSize, u64 arcSize, u16 nameLen, UTF-16LE */
    uint16_t nameLen = (uint16_t)data[subStart + 20] |
      ((uint16_t)data[subStart + 21] << 8);
    if ((uint64_t)subStart + 22 + nameLen * 2 > indexSize) return false;
    std::string utf8;
    if (!utf16leToUtf8(data + subStart + 22, (size_t)nameLen * 2, utf8))
      return false;
    Entry entry;
    if (!sanitizeName(utf8, entry.name)) {
      return fail(result, "unsafe entry name in archive index", utf8.c_str());
    }

    subStart = fileStart;
    subSize = fileSize;
    if (!findChunk(data, indexSize, tagSegm, subStart, subSize))
      return false;
    if (subSize % 28 != 0) return false;
    uint32_t segCount = subSize / 28;
    for (uint32_t i = 0; i < segCount; ++i) {
      const unsigned char *p = data + subStart + i * 28;
      Segment seg;
      seg.flags = rd32(p);
      if ((seg.flags & 0x07) != 0 && (seg.flags & 0x07) != 1) {
        return fail(result, "unsupported segment encoding in archive index",
          entry.name.c_str());
      }
      seg.offset = rd64(p + 4);
      seg.orgSize = rd64(p + 12);
      seg.arcSize = rd64(p + 20);
      entry.segments.push_back(seg);
    }
    if (entry.segments.empty()) {
      return fail(result, "file without segments in archive index",
        entry.name.c_str());
    }
    entries.push_back(entry);

    fileStart += fileSize;
    if (fileStart > indexSize) return false;
    fileSize = indexSize - fileStart;
  }
  return !entries.empty();
}

int extractSegments(FILE *f, const Entry &entry, const std::string &outPath,
  OHOSXp3ExtractResult *result) {
  FILE *out = fopen(outPath.c_str(), "wb");
  if (!out) {
    return fail(result, "cannot create output file", outPath.c_str());
  }
  std::vector<unsigned char> inBuf(1024 * 1024);
  std::vector<unsigned char> outBuf(1024 * 1024);
  int status = 0;
  for (size_t si = 0; si < entry.segments.size(); ++si) {
    const Segment &seg = entry.segments[si];
    if (fseeko(f, (off_t)seg.offset, SEEK_SET) != 0) {
      status = fail(result, "seek failed for segment", entry.name.c_str());
      break;
    }
    if ((seg.flags & 0x07) == 0) {
      /* raw copy */
      uint64_t remaining = seg.arcSize;
      while (remaining > 0) {
        size_t want = remaining > inBuf.size() ? inBuf.size() : (size_t)remaining;
        size_t got = fread(inBuf.data(), 1, want, f);
        if (got == 0) {
          status = fail(result, "truncated archive segment", entry.name.c_str());
          break;
        }
        if (fwrite(inBuf.data(), 1, got, out) != got) {
          status = fail(result, "write failed for output file", outPath.c_str());
          break;
        }
        remaining -= got;
      }
      if (status != 0) break;
    } else {
      /* zlib stream */
      z_stream zs;
      memset(&zs, 0, sizeof(zs));
      if (inflateInit(&zs) != Z_OK) {
        status = fail(result, "zlib init failed", entry.name.c_str());
        break;
      }
      uint64_t remaining = seg.arcSize;
      int zr = Z_OK;
      bool done = false;
      while (!done) {
        if (zs.avail_in == 0 && remaining > 0) {
          size_t want = remaining > inBuf.size() ? inBuf.size() : (size_t)remaining;
          size_t got = fread(inBuf.data(), 1, want, f);
          if (got == 0) {
            status = fail(result, "truncated compressed segment",
              entry.name.c_str());
            break;
          }
          zs.next_in = inBuf.data();
          zs.avail_in = (uInt)got;
          remaining -= got;
        }
        zs.next_out = outBuf.data();
        zs.avail_out = (uInt)outBuf.size();
        zr = inflate(&zs, Z_NO_FLUSH);
        size_t produced = outBuf.size() - zs.avail_out;
        if (produced > 0 && fwrite(outBuf.data(), 1, produced, out) != produced) {
          status = fail(result, "write failed for output file", outPath.c_str());
          break;
        }
        if (zr == Z_STREAM_END) {
          done = true;
        } else if (zr != Z_OK) {
          status = fail(result, "zlib decompression failed", entry.name.c_str());
          break;
        }
      }
      inflateEnd(&zs);
      if (status != 0) break;
    }
  }
  if (fclose(out) != 0 && status == 0) {
    status = fail(result, "close failed for output file", outPath.c_str());
  }
  if (status != 0) {
    unlink(outPath.c_str());
  }
  return status;
}

}  /* namespace */

int OHOS_ExtractXp3(const char *xp3Path, const char *outDir,
  OHOSXp3ProgressFn progress, void *ctx, OHOSXp3ExtractResult *result) {
  memset(result, 0, sizeof(*result));
  FILE *f = fopen(xp3Path, "rb");
  if (!f) return fail(result, "cannot open archive", xp3Path);

  int rc = 0;
  std::vector<Entry> entries;
  do {
    unsigned char sig[11];
    if (fread(sig, 1, 11, f) != 11 ||
      memcmp(sig, kSignature, 11) != 0) {
      rc = fail(result, "not an XP3 archive", xp3Path);
      break;
    }

    /* walk the index chain (supports the continue-bit chaining) */
    uint64_t indexOfs = 0;
    if (!read64At(f, 11, indexOfs)) {
      rc = fail(result, "cannot read index offset", xp3Path);
      break;
    }
    while (indexOfs != 0) {
      unsigned char flagByte = 0;
      if (!readAt(f, indexOfs, &flagByte, 1)) {
        rc = fail(result, "cannot read index flag", xp3Path);
        break;
      }
      uint64_t compressedSize = 0, indexSize = 0;
      uint64_t bodyOfs = 0;
      if ((flagByte & 0x07) == 1) {
        if (!read64At(f, indexOfs + 1, compressedSize) ||
          !read64At(f, indexOfs + 9, indexSize)) {
          rc = fail(result, "cannot read compressed index header", xp3Path);
          break;
        }
        bodyOfs = indexOfs + 17;
      } else if ((flagByte & 0x07) == 0) {
        if (!read64At(f, indexOfs + 1, indexSize)) {
          rc = fail(result, "cannot read index header", xp3Path);
          break;
        }
        bodyOfs = indexOfs + 9;
      } else {
        rc = fail(result, "unsupported index encoding", xp3Path);
        break;
      }
      if (indexSize > 0x7FFFFFFFULL) {
        rc = fail(result, "index too large", xp3Path);
        break;
      }
      std::vector<unsigned char> indexData((size_t)indexSize);
      uint64_t consumed = 0;
      if ((flagByte & 0x07) == 0) {
        if (!readAt(f, bodyOfs, indexData.data(), (size_t)indexSize)) {
          rc = fail(result, "cannot read index data", xp3Path);
          break;
        }
        consumed = bodyOfs + indexSize;
      } else {
        std::vector<unsigned char> compressed((size_t)compressedSize);
        if (!readAt(f, bodyOfs, compressed.data(), (size_t)compressedSize)) {
          rc = fail(result, "cannot read compressed index data", xp3Path);
          break;
        }
        uLongf destLen = (uLongf)indexSize;
        if (uncompress(indexData.data(), &destLen, compressed.data(),
          (uLong)compressedSize) != Z_OK || destLen != indexSize) {
          rc = fail(result, "index decompression failed", xp3Path);
          break;
        }
        consumed = bodyOfs + compressedSize;
      }
      if (rc != 0) break;
      if (!parseIndex(indexData.data(), (uint32_t)indexData.size(), entries,
        result)) {
        rc = -1;
        break;
      }
      /* continue-bit: the 8 bytes after the index body hold the next
       * block's offset (0 = end of chain) */
      uint64_t nextOfs = 0;
      read64At(f, consumed, nextOfs);
      indexOfs = (flagByte & 0x80) ? nextOfs : 0;
    }
    if (rc != 0) break;
    if (entries.empty()) {
      rc = fail(result, "archive index contains no files", xp3Path);
      break;
    }

    result->filesTotal = (int)entries.size();
    mkdir(outDir, 0777);
    if (!mkdirs(std::string(outDir) + "/.")) {
      rc = fail(result, "cannot create output directory", outDir);
      break;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
      std::string full = std::string(outDir) + "/" + entries[i].name;
      std::string dir = full;
      size_t slash = dir.find_last_of('/');
      if (slash != std::string::npos) {
        dir.resize(slash);
        if (!mkdirs(dir)) {
          rc = fail(result, "cannot create directory for",
            entries[i].name.c_str());
          break;
        }
      }
      int erc = extractSegments(f, entries[i], full, result);
      if (erc != 0) {
        rc = erc;
        break;
      }
      result->filesDone = (int)(i + 1);
      if (progress && !progress(ctx, result->filesDone, result->filesTotal,
        entries[i].name.c_str())) {
        rc = -2;
        break;
      }
    }
    if (rc == 0) {
      result->ok = 1;
    }
  } while (false);

  fclose(f);
  return rc;
}
