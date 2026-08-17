/* SPDX-License-Identifier: MIT */
/*
 * Extracts the packaged rawfile game content into the application sandbox.
 */

#ifndef OHOS_DATA_EXTRACT_H
#define OHOS_DATA_EXTRACT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Copy rawfile "data/**" into {filesDir}/data. Returns 1 on success. The
 * extraction is skipped when the extracted tree already matches the packaged
 * content-manifest.json. */
int OHOS_ExtractGameData(void);

#ifdef __cplusplus
}
#endif

#endif /* OHOS_DATA_EXTRACT_H */
