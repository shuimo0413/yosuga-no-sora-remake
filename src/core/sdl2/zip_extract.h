/* SPDX-License-Identifier: MIT */
/*
 * Minimal ZIP archive extractor for the iOS bootstrap flow.
 *
 * Reads the ZIP central directory (including the ZIP64 end records the
 * Python packer may emit) and inflates every entry into outDir. Only the
 * zlib deflate and stored methods are supported, which covers the archives
 * produced by tools/package_data_release.py (python zipfile).
 */

#ifndef ZIP_EXTRACT_H
#define ZIP_EXTRACT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback: called after each extracted file. Return 0 to cancel.
 * nameUtf8 is the in-archive path of the last completed file. */
typedef int (*KrkrZipProgressFn)(void *ctx, int done, int total,
  const char *nameUtf8);

/* Extracts zipPath into outDir (created if missing). Path traversal is
 * blocked (entries whose resolved path escapes outDir are skipped).
 * Returns 0 on success, -1 on failure (err is filled), -2 when cancelled. */
int Krkr_ExtractZip(const char *zipPath, const char *outDir,
  KrkrZipProgressFn progress, void *ctx, char *err, size_t errSize);

#ifdef __cplusplus
}
#endif

#endif /* ZIP_EXTRACT_H */
