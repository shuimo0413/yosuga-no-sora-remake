/* SPDX-License-Identifier: MIT */
/*
 * Self-contained XP3 archive extractor for the import flow.
 *
 * Reads the krkrz XP3 container format (version 2) exactly the way the
 * engine base/XP3Archive.cpp does: 11-byte signature, uint64 index offset,
 * an index of tagged chunks (File chunks containing info/segm/adlr
 * sub-chunks) and per-segment raw/zlib encoding flags. File names in the
 * index are UTF-16LE; segments are extracted in order into the output
 * directory. No TVP initialization is required - only stdio and zlib.
 */

#ifndef OHOS_XP3_EXTRACT_H
#define OHOS_XP3_EXTRACT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OHOSXp3ExtractResult {
  int ok;          /* 1 when the whole archive was extracted */
  int filesDone;
  int filesTotal;
  char error[512]; /* UTF-8 message when ok == 0 */
} OHOSXp3ExtractResult;

/* Called from the worker thread after each extracted file (or a batch of
 * them). Return 0 to cancel the extraction. nameUtf8 is the in-archive
 * path of the last completed file. */
typedef int (*OHOSXp3ProgressFn)(void *ctx, int done, int total,
  const char *nameUtf8);

/* Extracts xp3Path into outDir (created if missing). The caller extracts
 * into a temporary directory and renames it into place afterwards so a
 * crash can never leave a half-extracted tree that looks complete.
 * Returns 0 on success, -1 on failure (result->error is filled),
 * -2 when cancelled. */
int OHOS_ExtractXp3(const char *xp3Path, const char *outDir,
  OHOSXp3ProgressFn progress, void *ctx, OHOSXp3ExtractResult *result);

#ifdef __cplusplus
}
#endif

#endif /* OHOS_XP3_EXTRACT_H */
