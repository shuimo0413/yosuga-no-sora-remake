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
#include <new>
#include <algorithm>
#include <map>
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
    if (arg && arg[0]) {
      snprintf(result->error, sizeof(result->error), "%s: %s", fmt, arg);
    } else {
      snprintf(result->error, sizeof(result->error), "%s", fmt);
    }
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

/* Locate a chunk tagged with the given 4-byte tag inside the absolute
 * range data[start, start+size). On a match sets start to the content
 * start and size to the content size; otherwise scans forward (mirrors
 * tTVPXP3Archive::FindChunk). */
bool findChunk(const unsigned char *data, uint32_t indexSize,
  const unsigned char tag[4], uint32_t &start, uint32_t &size) {
  uint32_t end = start + size;   /* absolute end of the search range */
  uint32_t pos = start;
  while (pos < end) {
    if (pos + 12 > end) return false;
    bool match = memcmp(data + pos, tag, 4) == 0;
    uint64_t chunkSize = rd64(data + pos + 4);
    if (chunkSize > 0x7FFFFFFFULL) return false;
    /* the chunk body must stay inside the search range */
    if ((uint64_t)pos + 12 + chunkSize > end) return false;
    if (match) {
      start = pos + 12;
      size = (uint32_t)chunkSize;
      return true;
    }
    pos += 12 + (uint32_t)chunkSize;
  }
  return false;
}

/* Parse one File chunk (content at data[start..start+size)) into an Entry.
 * Mirrors tTVPXP3Archive's info/segm sub-chunk parsing. */
bool parseEntry(const unsigned char *data, uint32_t indexSize, uint32_t start,
  uint32_t size, Entry &entry, OHOSXp3ExtractResult *result) {
  const unsigned char tagInfo[4] = { 0x69, 0x6e, 0x66, 0x6f };
  const unsigned char tagSegm[4] = { 0x73, 0x65, 0x67, 0x6d };
  uint32_t subStart = start;
  uint32_t subSize = size;
  if (!findChunk(data, indexSize, tagInfo, subStart, subSize)) {
    return fail(result, "archive entry without info chunk", "");
  }
  if (subSize < 22) {
    return fail(result, "archive info chunk too small", "");
  }
  /* info: u32 flags, u64 orgSize, u64 arcSize, u16 nameLen, UTF-16LE */
  uint16_t nameLen = (uint16_t)data[subStart + 20] |
    ((uint16_t)data[subStart + 21] << 8);
  if ((uint64_t)subStart + 22 + nameLen * 2 > indexSize) {
    return fail(result, "archive entry name overflows the index", "");
  }
  std::string utf8;
  if (!utf16leToUtf8(data + subStart + 22, (size_t)nameLen * 2, utf8)) {
    return fail(result, "cannot decode archive entry name", "");
  }
  if (!sanitizeName(utf8, entry.name)) {
    return fail(result, "unsafe entry name in archive index", utf8.c_str());
  }

  subStart = start;
  subSize = size;
  if (!findChunk(data, indexSize, tagSegm, subStart, subSize)) {
    return fail(result, "archive entry without segm chunk", entry.name.c_str());
  }
  if (subSize % 28 != 0) {
    return fail(result, "archive segm chunk size not a multiple of 28",
      entry.name.c_str());
  }
  uint32_t segCount = subSize / 28;
  if (segCount == 0) {
    return fail(result, "file without segments in archive index",
      entry.name.c_str());
  }
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
  return true;
}

bool parseIndex(const unsigned char *data, uint32_t indexSize,
  std::vector<Entry> &entries, OHOSXp3ExtractResult *result) {
  const unsigned char tagFile[4] = { 0x46, 0x69, 0x6c, 0x65 };
  uint32_t fileStart = 0;
  uint32_t fileSize = indexSize;
  for (;;) {
    if (!findChunk(data, indexSize, tagFile, fileStart, fileSize)) break;
    Entry entry;
    if (!parseEntry(data, indexSize, fileStart, fileSize, entry, result))
      return false;
    entries.push_back(entry);

    fileStart += fileSize;
    if (fileStart > indexSize) {
      return fail(result, "archive index entries overlap the index end", "");
    }
    fileSize = indexSize - fileStart;
  }
  if (entries.empty()) {
    return fail(result, "archive index contains no file entries", "");
  }
  return true;
}

/* Some packers put a decoy header at the front of the file and the REAL
 * index as a plain contiguous File-chunk chain in the last few megabytes
 * (the chain ends exactly at EOF). Recover it by scanning the tail. */
/* Some packers (GARbro XP3 v2 among others) put a decoy header block at
 * the front of the file and the REAL index as a plain contiguous
 * File-chunk chain in the last few megabytes (the chain ends at EOF).
 * Recover it by scanning the tail. */
bool carveTailIndex(FILE *f, uint64_t fileSize,
  std::vector<Entry> &entries, OHOSXp3ExtractResult *result) {
  const uint64_t scanCap = 64 * 1024 * 1024ULL;
  uint64_t scanLen = fileSize < scanCap ? fileSize : scanCap;
  uint64_t scanBase = fileSize - scanLen;
  std::vector<unsigned char> tail((size_t)scanLen);
  if (!readAt(f, scanBase, tail.data(), (size_t)scanLen)) {
    return fail(result, "cannot read archive tail for index recovery", "");
  }

  struct Cand { uint64_t pos; uint32_t csize; };
  std::vector<Cand> cands;
  for (uint64_t p = 0; p + 12 <= scanLen; ++p) {
    if (memcmp(tail.data() + p, "File", 4) != 0) continue;
    uint64_t csize = rd64(tail.data() + p + 4);
    if (csize < 22 || csize > 1024 * 1024) continue;
    if (p + 12 + csize > scanLen) continue;
    cands.push_back({ scanBase + p, (uint32_t)csize });
  }
  if (cands.size() < 2) {
    return fail(result, "no index chain found in archive tail", "");
  }

  /* chains run forward (a chunk starts where the previous one ended);
   * collect every chain whose end lies within a few bytes of the EOF by
   * walking backwards, then try them longest-first */
  std::map<uint64_t, size_t> endToIdx;
  for (size_t i = 0; i < cands.size(); ++i) {
    endToIdx[cands[i].pos + 12 + cands[i].csize] = i;
  }

  std::vector<std::vector<size_t>> chains;
  std::map<uint64_t, size_t>::iterator it = endToIdx.end();
  while (it != endToIdx.begin()) {
    --it;
    uint64_t end = it->first;
    if (end > fileSize || fileSize - end > 4096) continue;
    std::vector<size_t> chain;
    uint64_t curEnd = end;
    size_t steps = 0;
    while (steps++ < cands.size()) {
      std::map<uint64_t, size_t>::iterator back = endToIdx.find(curEnd);
      if (back == endToIdx.end()) break;
      size_t idx = back->second;
      chain.push_back(idx);
      curEnd = cands[idx].pos;
    }
    chains.push_back(chain);
  }
  if (chains.empty()) {
    return fail(result, "no index chain ends at the archive tail", "");
  }
  std::sort(chains.begin(), chains.end(),
    [](const std::vector<size_t> &a, const std::vector<size_t> &b) {
      return a.size() > b.size();
    });

  for (size_t ci = 0; ci < chains.size(); ++ci) {
    const std::vector<size_t> &chain = chains[ci];
    if (chain.size() < 2) continue;
    std::vector<Entry> trial;
    bool ok = true;
    /* chain is collected tail-to-head; parse head-to-tail */
    for (size_t t = chain.size(); t-- > 0;) {
      const Cand &c = cands[chain[t]];
      Entry entry;
      if (!parseEntry(tail.data(), (uint32_t)tail.size(),
        (uint32_t)(c.pos - scanBase), c.csize, entry, result)) {
        ok = false;
        break;
      }
      trial.push_back(entry);
    }
    if (ok && !trial.empty()) {
      entries.swap(trial);
      return true;
    }
  }
  return fail(result, "no parseable index chain found in archive tail", "");
}


int extractSegments(FILE *f, const Entry &entry, const std::string &outPath,
  OHOSXp3ExtractResult *result, FILE *diag) {
  FILE *out = fopen(outPath.c_str(), "wb");
  if (!out) {
    if (diag) {
      fprintf(diag, "extract fopen failed: %s (errno=%d %s)%c",
        outPath.c_str(), errno, strerror(errno), (char)10);
      fflush(diag);
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "cannot create output file (errno=%d %s)",
      errno, strerror(errno));
    return fail(result, msg, outPath.c_str());
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

  /* stage-by-stage diagnostic next to the output (the ArkTS side reads it
   * back into debug.log when something fails; hdc cannot reach the public
   * folder) */
  std::string diagPath = std::string(outDir) + ".diag";
  FILE *diag = fopen(diagPath.c_str(), "w");
  if (diag) {
    fprintf(diag, "xp3=%s\n", xp3Path);
    fprintf(diag, "outDir=%s\n", outDir);
    fflush(diag);
  }

  int rc = 0;
  std::vector<Entry> entries;
  try {
    do {
    unsigned char sig[11];
    if (fread(sig, 1, 11, f) != 11 ||
      memcmp(sig, kSignature, 11) != 0) {
      rc = fail(result, "not an XP3 archive", xp3Path);
      if (diag) { fprintf(diag, "signature mismatch\n"); fflush(diag); }
      break;
    }
    if (diag) { fprintf(diag, "signature ok\n"); fflush(diag); }

    /* Get the real file size first: both the tail-index recovery and the
     * segment validation below need it. */
    fseeko(f, 0, SEEK_END);
    uint64_t archiveSize = (uint64_t)ftello(f);
    if (diag) {
      fprintf(diag, "archiveSize=%llu\n", (unsigned long long)archiveSize);
      fflush(diag);
    }

    /* Walk the standard index chain (continue-bit chaining). The chain is
     * ABANDONED - not failed - when it turns out to be a decoy: GARbro's
     * XP3 v2 puts an empty stub block at the front and the real index at
     * the end of the file, so an empty or unparsable block falls back to
     * tail recovery below. */
    bool chainOk = true;
    uint64_t indexOfs = 0;
    if (!read64At(f, 11, indexOfs)) {
      chainOk = false;
    }
    if (diag) {
      fprintf(diag, "indexOfs=%llu\n", (unsigned long long)indexOfs);
      fflush(diag);
    }
    while (chainOk && indexOfs != 0) {
      unsigned char flagByte = 0;
      if (!readAt(f, indexOfs, &flagByte, 1)) {
        chainOk = false;
        break;
      }
      if (diag) {
        fprintf(diag, "block@%llu flag=0x%02x\n",
          (unsigned long long)indexOfs, flagByte);
        fflush(diag);
      }
      uint64_t compressedSize = 0, indexSize = 0;
      uint64_t bodyOfs = 0;
      if ((flagByte & 0x07) == 1) {
        if (!read64At(f, indexOfs + 1, compressedSize) ||
          !read64At(f, indexOfs + 9, indexSize)) {
          chainOk = false;
          break;
        }
        bodyOfs = indexOfs + 17;
      } else if ((flagByte & 0x07) == 0) {
        if (!read64At(f, indexOfs + 1, indexSize)) {
          chainOk = false;
          break;
        }
        bodyOfs = indexOfs + 9;
      } else {
        /* unknown encoding: decoy/foreign block */
        chainOk = false;
        break;
      }
      if (indexSize == 0) {
        /* empty stub block (GARbro style): the real index is in the tail */
        if (diag) { fprintf(diag, "empty stub block -> tail recovery\n"); fflush(diag); }
        chainOk = false;
        break;
      }
      /* 256 MB is far beyond any real index (a 4 GB archive with 23k
       * files has a ~5 MB index); larger values mean a mis-parsed header,
       * which would otherwise trigger a huge (and fatal) allocation. */
      if (indexSize > 256 * 1024 * 1024ULL ||
        compressedSize > 256 * 1024 * 1024ULL) {
        chainOk = false;
        break;
      }
      std::vector<unsigned char> indexData((size_t)indexSize);
      uint64_t consumed = 0;
      if ((flagByte & 0x07) == 0) {
        if (!readAt(f, bodyOfs, indexData.data(), (size_t)indexSize)) {
          chainOk = false;
          break;
        }
        consumed = bodyOfs + indexSize;
      } else {
        std::vector<unsigned char> compressed((size_t)compressedSize);
        if (!readAt(f, bodyOfs, compressed.data(), (size_t)compressedSize)) {
          chainOk = false;
          break;
        }
        uLongf destLen = (uLongf)indexSize;
        if (uncompress(indexData.data(), &destLen, compressed.data(),
          (uLong)compressedSize) != Z_OK || destLen != indexSize) {
          chainOk = false;
          break;
        }
        consumed = bodyOfs + compressedSize;
      }
      if (!parseIndex(indexData.data(), (uint32_t)indexData.size(), entries,
        result)) {
        /* a broken chain means the index is not what we think it is:
         * recover from the tail instead of trusting a partial index */
        entries.clear();
        chainOk = false;
        break;
      }
      /* continue-bit: the 8 bytes after the index body hold the next
       * block's offset (0 = end of chain) */
      uint64_t nextOfs = 0;
      read64At(f, consumed, nextOfs);
      indexOfs = (flagByte & 0x80) ? nextOfs : 0;
    }
    if (chainOk && entries.empty()) {
      chainOk = false;
    }

    if (!chainOk) {
      /* Recover the real index from the file tail: GARbro and friends keep
       * a plain contiguous File-chunk chain that ends exactly at EOF. */
      if (diag) { fprintf(diag, "falling back to tail recovery\n"); fflush(diag); }
      entries.clear();
      result->error[0] = 0;
      if (!carveTailIndex(f, archiveSize, entries, result)) {
        if (diag) {
          fprintf(diag, "tail recovery failed: %s\n", result->error);
          fflush(diag);
        }
        rc = -1;
        break;
      }
      if (diag) {
        fprintf(diag, "tail recovery found %zu entries\n", entries.size());
        fflush(diag);
      }
    }
    if (entries.empty()) {
      rc = fail(result, "archive index contains no files", xp3Path);
      break;
    }

    /* validate every segment against the real archive size before
     * copying anything (a mis-parsed index must not seek into the void) */
    for (size_t i = 0; i < entries.size() && rc == 0; ++i) {
      for (size_t si = 0; si < entries[i].segments.size(); ++si) {
        const Segment &seg = entries[i].segments[si];
        if (seg.offset > archiveSize ||
          seg.offset + seg.arcSize > archiveSize) {
          rc = fail(result, "segment offset out of archive bounds",
            entries[i].name.c_str());
          break;
        }
      }
    }
    if (rc != 0) break;

    if (diag) {
      fprintf(diag, "entries=%zu (filesTotal=%d)\n",
        entries.size(), (int)entries.size());
      fflush(diag);
    }

    result->filesTotal = (int)entries.size();
    mkdir(outDir, 0777);
    if (!mkdirs(std::string(outDir) + "/.")) {
      rc = fail(result, "cannot create output directory", outDir);
      if (diag) {
        fprintf(diag, "mkdirs(%s) failed (errno=%d %s)\n", outDir,
          errno, strerror(errno));
        fflush(diag);
      }
      break;
    }
    if (diag) { fprintf(diag, "output dir ready\n"); fflush(diag); }

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
      int erc = extractSegments(f, entries[i], full, result, diag);
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
  } catch (const std::bad_alloc &) {
    rc = fail(result, "out of memory while extracting", xp3Path);
  } catch (...) {
    rc = fail(result, "internal error: unexpected exception", xp3Path);
  }

  if (diag) {
    fprintf(diag, "final rc=%d ok=%d files=%d/%d error=%s\n", rc,
      result->ok, result->filesDone, result->filesTotal, result->error);
    fflush(diag);
    fclose(diag);
  }
  fclose(f);
  return rc;
}
