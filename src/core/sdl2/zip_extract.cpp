/* SPDX-License-Identifier: MIT */
/*
 * Minimal ZIP archive extractor (central directory based, ZIP64 aware).
 * Only stdio and zlib. The zlib objects live inside the krkrsdl2 target
 * (external/krkrz/external/zlib), so the symbols resolve at link time.
 */

#include "zip_extract.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

struct EndRecord {
  uint64_t cdOffset = 0;
  uint64_t cdSize = 0;
  uint64_t cdCount = 0;
  int zip64 = 0;
};

bool ReadLE16(FILE *f, uint16_t *v)
{
  unsigned char b[2];
  if (fread(b, 1, 2, f) != 2) return false;
  *v = (uint16_t)(b[0] | (b[1] << 8));
  return true;
}

bool ReadLE32(FILE *f, uint32_t *v)
{
  unsigned char b[4];
  if (fread(b, 1, 4, f) != 4) return false;
  *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

bool ReadLE64(FILE *f, uint64_t *v)
{
  unsigned char b[8];
  if (fread(b, 1, 8, f) != 8) return false;
  uint64_t r = 0;
  for (int i = 7; i >= 0; --i) r = (r << 8) | b[i];
  *v = r;
  return true;
}

/* Locate the End Of Central Directory record (and ZIP64 EOCD when present)
 * by scanning backwards from the end of the file for the 0x06054b50
 * signature. */
bool FindEndRecord(FILE *f, EndRecord *out)
{
  long fileSize = 0;
  if (fseek(f, 0, SEEK_END) != 0) return false;
  fileSize = ftell(f);
  if (fileSize < 22) return false;

  const long scanMax = 22 + 65536;
  long scan = fileSize < scanMax ? fileSize : scanMax;
  std::vector<unsigned char> tail((size_t)scan);
  if (fseek(f, fileSize - scan, SEEK_SET) != 0) return false;
  if (fread(tail.data(), 1, (size_t)scan, f) != (size_t)scan) return false;

  long eocdPos = -1;
  for (long i = (long)scan - 22; i >= 0; --i)
  {
    if (tail[i] == 0x50 && tail[i + 1] == 0x4b &&
      tail[i + 2] == 0x05 && tail[i + 3] == 0x06)
    {
      eocdPos = fileSize - scan + i;
      break;
    }
  }
  if (eocdPos < 0) return false;

  if (fseek(f, eocdPos + 4, SEEK_SET) != 0) return false;
  uint16_t disk = 0, cdDisk = 0, diskCount = 0, cdCount16 = 0;
  uint32_t cdSize32 = 0, cdOffset32 = 0;
  uint16_t commentLen = 0;
  if (!ReadLE16(f, &disk) || !ReadLE16(f, &cdDisk) ||
    !ReadLE16(f, &diskCount) || !ReadLE16(f, &cdCount16) ||
    !ReadLE32(f, &cdSize32) || !ReadLE32(f, &cdOffset32) ||
    !ReadLE16(f, &commentLen))
    return false;

  (void)disk;
  (void)diskCount;
  out->cdSize = cdSize32;
  out->cdOffset = cdOffset32;
  out->cdCount = cdCount16;

  /* ZIP64: sentinel values in the EOCD point at the ZIP64 EOCD locator,
   * which directly precedes the regular EOCD. */
  if (cdCount16 == 0xFFFF || cdSize32 == 0xFFFFFFFF ||
    cdOffset32 == 0xFFFFFFFF)
  {
    const long locatorPos = eocdPos - 20;
    if (locatorPos >= 0)
    {
      if (fseek(f, locatorPos, SEEK_SET) == 0)
      {
        uint32_t sig = 0;
        uint64_t zip64EocdOff = 0;
        if (ReadLE32(f, &sig) && sig == 0x07064b50)
        {
          ReadLE32(f, &sig); /* disk number */
          if (ReadLE64(f, &zip64EocdOff) && zip64EocdOff > 0)
          {
            if (fseek(f, (long)zip64EocdOff, SEEK_SET) == 0)
            {
              if (ReadLE32(f, &sig) && sig == 0x06064b50)
              {
                uint64_t recSize = 0;
                ReadLE64(f, &recSize);
                uint16_t madeBy = 0, needed = 0;
                ReadLE16(f, &madeBy);
                ReadLE16(f, &needed);
                uint32_t disk32 = 0, cdDisk32 = 0;
                ReadLE32(f, &disk32);
                ReadLE32(f, &cdDisk32);
                uint64_t diskCount64 = 0, cdCount64 = 0;
                uint64_t cdSize64 = 0, cdOffset64 = 0;
                ReadLE64(f, &diskCount64);
                ReadLE64(f, &cdCount64);
                ReadLE64(f, &cdSize64);
                ReadLE64(f, &cdOffset64);
                (void)madeBy; (void)needed;
                (void)disk32; (void)cdDisk32; (void)diskCount64;
                if (cdCount64 > 0 && cdOffset64 > 0)
                {
                  out->cdSize = cdSize64;
                  out->cdOffset = cdOffset64;
                  out->cdCount = cdCount64;
                  out->zip64 = 1;
                }
              }
            }
          }
        }
      }
    }
  }
  return true;
}

struct Entry {
  std::string name;
  int method = 0;
  uint64_t compressedSize = 0;
  uint64_t uncompressedSize = 0;
  uint32_t crc32 = 0;
  uint64_t localHeaderOffset = 0;
};

/* Read one central directory header; returns false at end or on error. */
bool ReadCentralEntry(FILE *f, Entry *e)
{
  uint32_t sig = 0;
  if (!ReadLE32(f, &sig)) return false;
  if (sig == 0x06054b50) return false; /* EOCD reached */
  if (sig == 0x06064b50) return false; /* ZIP64 EOCD */
  if (sig != 0x02014b50)
  {
    /* skip unknown record */
    return false;
  }
  uint16_t madeBy = 0, needed = 0, flags = 0, method16 = 0;
  uint16_t modTime = 0, modDate = 0;
  uint32_t crc = 0, comp32 = 0, uncomp32 = 0;
  uint16_t nameLen = 0, extraLen = 0, commentLen = 0;
  uint16_t diskStart = 0, intAttr = 0;
  uint32_t extAttr = 0, localOff32 = 0;
  if (!ReadLE16(f, &madeBy) || !ReadLE16(f, &needed) ||
    !ReadLE16(f, &flags) || !ReadLE16(f, &method16) ||
    !ReadLE16(f, &modTime) || !ReadLE16(f, &modDate) ||
    !ReadLE32(f, &crc) || !ReadLE32(f, &comp32) ||
    !ReadLE32(f, &uncomp32) || !ReadLE16(f, &nameLen) ||
    !ReadLE16(f, &extraLen) || !ReadLE16(f, &commentLen) ||
    !ReadLE16(f, &diskStart) || !ReadLE16(f, &intAttr) ||
    !ReadLE32(f, &extAttr) || !ReadLE32(f, &localOff32))
    return false;
  (void)madeBy; (void)needed; (void)modTime; (void)modDate;
  (void)diskStart; (void)intAttr; (void)extAttr;

  e->method = method16;
  e->compressedSize = comp32;
  e->uncompressedSize = uncomp32;
  e->crc32 = crc;
  e->localHeaderOffset = localOff32;

  std::vector<char> nameBuf((size_t)nameLen + 1);
  if (fread(nameBuf.data(), 1, nameLen, f) != nameLen) return false;
  nameBuf[nameLen] = 0;
  e->name.assign(nameBuf.data(), nameLen);

  /* ZIP64 extra field (0x0001) holds the real sizes/offset. */
  std::vector<unsigned char> extra((size_t)extraLen);
  if (extraLen > 0 && fread(extra.data(), 1, extraLen, f) != extraLen)
    return false;
  size_t pos = 0;
  while (pos + 4 <= extra.size())
  {
    uint16_t id = (uint16_t)(extra[pos] | (extra[pos + 1] << 8));
    uint16_t sz = (uint16_t)(extra[pos + 2] | (extra[pos + 3] << 8));
    pos += 4;
    if (pos + sz > extra.size()) break;
    if (id == 0x0001)
    {
      size_t p = pos;
      if (e->uncompressedSize == 0xFFFFFFFF && p + 8 <= extra.size())
      {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | extra[p + i];
        e->uncompressedSize = v;
        p += 8;
      }
      if (e->compressedSize == 0xFFFFFFFF && p + 8 <= extra.size())
      {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | extra[p + i];
        e->compressedSize = v;
        p += 8;
      }
      if (e->localHeaderOffset == 0xFFFFFFFF && p + 8 <= extra.size())
      {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | extra[p + i];
        e->localHeaderOffset = v;
      }
      break;
    }
    pos += sz;
  }

  if (commentLen > 0 && fseek(f, commentLen, SEEK_CUR) != 0) return false;
  return true;
}

std::string SafeJoin(const std::string &dir, const std::string &rel)
{
  std::string out = dir;
  if (!out.empty() && out.back() != '/') out += '/';
  out += rel;
  return out;
}

/* Block path traversal and absolute paths; entries are data/* from the
 * packer, but user-supplied zips may contain anything. */
bool IsSafeEntryName(const std::string &name, std::string *clean)
{
  clean->clear();
  if (name.empty()) return false;
  if (name[0] == '/' || name[0] == '\\') return false;
  if (name.size() >= 2 && name[1] == ':') return false;
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= name.size())
  {
    size_t slash = name.find('/', start);
    std::string part = slash == std::string::npos
      ? name.substr(start) : name.substr(start, slash - start);
    if (part == "..") return false;
    if (!part.empty() && part != ".")
    {
      if (!clean->empty()) *clean += '/';
      *clean += part;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return !clean->empty();
}

bool Mkdirs(const std::string &path, std::string *errOut)
{
  /* Keep a leading slash so absolute paths (the iOS sandbox) are built
   * correctly: building "var/mobile/..." relative to the CWD put every
   * directory in the wrong place and fopen() failed with "cannot create". */
  std::string cur;
  size_t start = 0;
  if (!path.empty() && path[0] == '/')
  {
    cur = "/";
    start = 1;
  }
  while (start <= path.size())
  {
    size_t slash = path.find('/', start);
    std::string part = slash == std::string::npos
      ? path.substr(start) : path.substr(start, slash - start);
    if (!part.empty())
    {
      if (!cur.empty() && cur.back() != '/') cur += '/';
      cur += part;
#if defined(_WIN32)
      if (_mkdir(cur.c_str()) != 0 && errno != EEXIST && errOut)
#else
      if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST && errOut)
#endif
      {
        if (errOut->empty())
          *errOut = "mkdir failed for '" + cur + "' errno=" +
            std::to_string(errno);
      }
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return errOut == NULL || errOut->empty();
}

bool CopyStored(FILE *in, uint64_t size, FILE *out)
{
  unsigned char buf[32 * 1024];
  while (size > 0)
  {
    size_t chunk = size > sizeof(buf) ? sizeof(buf) : (size_t)size;
    if (fread(buf, 1, chunk, in) != chunk) return false;
    if (fwrite(buf, 1, chunk, out) != chunk) return false;
    size -= chunk;
  }
  return true;
}

bool InflateEntry(FILE *in, uint64_t compressedSize, uint64_t uncompressedSize,
  FILE *out, std::string *err)
{
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, -15) != Z_OK)
  {
    *err = "inflateInit2 failed";
    return false;
  }
  /* 32 KB buffers keep the combined frame small: this also runs on the
   * GCD worker thread whose stack is ~512 KB (a 1 MB stack buffer in the
   * sha256 path already caused a "Thread stack size exceeded" SIGBUS). */
  unsigned char inBuf[32 * 1024];
  unsigned char outBuf[32 * 1024];
  uint64_t remaining = compressedSize;
  uint64_t produced = 0;
  bool ok = true;
  int zr = Z_OK;
  while (remaining > 0 || zr != Z_STREAM_END)
  {
    if (zs.avail_in == 0 && remaining > 0)
    {
      size_t chunk = remaining > sizeof(inBuf) ? sizeof(inBuf)
        : (size_t)remaining;
      if (fread(inBuf, 1, chunk, in) != chunk)
      {
        ok = false;
        *err = "read failed";
        break;
      }
      zs.next_in = inBuf;
      zs.avail_in = (uInt)chunk;
      remaining -= chunk;
    }
    zs.next_out = outBuf;
    zs.avail_out = sizeof(outBuf);
    zr = inflate(&zs, Z_NO_FLUSH);
    if (zr != Z_OK && zr != Z_STREAM_END)
    {
      ok = false;
      *err = "inflate failed";
      break;
    }
    size_t have = sizeof(outBuf) - zs.avail_out;
    if (have > 0)
    {
      if (fwrite(outBuf, 1, have, out) != have)
      {
        ok = false;
        *err = "write failed";
        break;
      }
      produced += have;
    }
    if (zr == Z_STREAM_END) break;
  }
  inflateEnd(&zs);
  (void)uncompressedSize;
  (void)produced;
  return ok;
}

} // namespace

int Krkr_ExtractZip(const char *zipPath, const char *outDir,
  KrkrZipProgressFn progress, void *ctx, char *err, size_t errSize)
{
  if (err && errSize > 0) err[0] = 0;
  FILE *f = fopen(zipPath, "rb");
  if (!f)
  {
    if (err) snprintf(err, errSize, "cannot open %s", zipPath);
    return -1;
  }

  EndRecord end;
  if (!FindEndRecord(f, &end))
  {
    fclose(f);
    if (err) snprintf(err, errSize, "no end-of-central-directory record");
    return -1;
  }

  /* Collect the central directory. */
  std::vector<Entry> entries;
  if (fseek(f, (long)end.cdOffset, SEEK_SET) != 0)
  {
    fclose(f);
    if (err) snprintf(err, errSize, "cannot seek to central directory");
    return -1;
  }
  uint64_t readCount = 0;
  while (readCount < end.cdCount)
  {
    Entry e;
    if (!ReadCentralEntry(f, &e)) break;
    entries.push_back(e);
    readCount++;
  }

  /* Skip directories, count files. */
  std::vector<const Entry *> files;
  for (size_t i = 0; i < entries.size(); ++i)
  {
    const Entry &e = entries[i];
    if (!e.name.empty() && e.name.back() != '/') files.push_back(&e);
  }
  const int total = (int)files.size();

  int done = 0;
  int rc = 0;
  std::string errStr;
  if (!Mkdirs(outDir, &errStr))
  {
    fclose(f);
    if (err) snprintf(err, errSize, "%s", errStr.c_str());
    return -1;
  }
  for (size_t i = 0; i < files.size(); ++i)
  {
    const Entry &e = *files[i];
    std::string clean;
    if (!IsSafeEntryName(e.name, &clean))
    {
      continue; /* skip unsafe entry, keep going */
    }
    std::string target = SafeJoin(outDir, clean);
    /* Create only the PARENT directory: Mkdirs on the full target also
     * created the file name itself as a directory, so fopen() then failed
     * with errno=21 (Is a directory). */
    size_t lastSlash = target.find_last_of('/');
    std::string parent = (lastSlash == std::string::npos)
      ? outDir : target.substr(0, lastSlash);
    if (!Mkdirs(parent, &errStr))
    {
      rc = -1;
      break;
    }

    if (fseek(f, (long)e.localHeaderOffset, SEEK_SET) != 0)
    {
      errStr = "cannot seek to local header of " + e.name;
      rc = -1;
      break;
    }
    uint32_t sig = 0;
    if (!ReadLE32(f, &sig) || sig != 0x04034b50)
    {
      errStr = "bad local header for " + e.name;
      rc = -1;
      break;
    }
    /* local header: skip to the data start */
    uint16_t ver = 0, flags = 0, method16 = 0, modTime = 0, modDate = 0;
    uint32_t crc = 0, comp32 = 0, uncomp32 = 0;
    uint16_t nameLen = 0, extraLen = 0;
    if (!ReadLE16(f, &ver) || !ReadLE16(f, &flags) ||
      !ReadLE16(f, &method16) || !ReadLE16(f, &modTime) ||
      !ReadLE16(f, &modDate) || !ReadLE32(f, &crc) ||
      !ReadLE32(f, &comp32) || !ReadLE32(f, &uncomp32) ||
      !ReadLE16(f, &nameLen) || !ReadLE16(f, &extraLen))
    {
      errStr = "bad local header fields for " + e.name;
      rc = -1;
      break;
    }
    (void)ver; (void)method16; (void)modTime; (void)modDate;
    (void)crc; (void)comp32; (void)uncomp32;
    if (fseek(f, (long)nameLen + extraLen, SEEK_CUR) != 0)
    {
      errStr = "seek past local header failed for " + e.name;
      rc = -1;
      break;
    }

    FILE *out = fopen(target.c_str(), "wb");
    if (!out)
    {
      errStr = "cannot create " + target + " (errno=" +
        std::to_string(errno) + " " + strerror(errno) + ")";
      rc = -1;
      break;
    }
    bool ok = e.method == 0
      ? CopyStored(f, e.compressedSize, out)
      : (e.method == 8
        ? InflateEntry(f, e.compressedSize, e.uncompressedSize, out, &errStr)
        : false);
    fclose(out);
    if (!ok)
    {
      if (errStr.empty()) errStr = "unsupported compression method " +
        std::to_string(e.method) + " for " + e.name;
      rc = -1;
      break;
    }
    done++;
    if (progress)
    {
      if (!progress(ctx, done, total, e.name.c_str()))
      {
        rc = -2;
        break;
      }
    }
  }

  fclose(f);
  if (rc != 0 && err && errSize > 0)
  {
    snprintf(err, errSize, "%s", errStr.empty() ? "zip extraction failed"
      : errStr.c_str());
  }
  return rc;
}
