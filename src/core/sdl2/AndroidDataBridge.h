/* SPDX-License-Identifier: MIT */
/* Android external data directory accessor for StorageImpl.cpp. */
#ifndef ANDROID_DATA_BRIDGE_H
#define ANDROID_DATA_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the extracted data root set by the bootstrap activity through
 * nativeSetDataDir, or null when the game data lives in the APK assets. */
const char *AndroidDataDir_Get(void);

#ifdef __cplusplus
}
#endif

#endif /* ANDROID_DATA_BRIDGE_H */
