/* SPDX-License-Identifier: MIT */
/*
 * iOS bootstrap page (data externalization).
 *
 * Runs before the engine starts: probes the external data directory
 * (Documents/<bundle>/data), and when the game data is missing shows the
 * bootstrap UI (download from the release assets / import a local zip or
 * data.xp3), mirroring the OpenHarmony shell page.
 */

#ifndef IOS_BOOTSTRAP_H
#define IOS_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the bootstrap flow on the main thread (SDL_main context). Returns 1
 * when the game data is ready and the engine should start, 0 when the app
 * should terminate. */
int krkrsdl2_ios_run_bootstrap(void);

/* External data root: Documents/<bundle-id> (UTF-8, no trailing slash).
 * The engine resolves ./data/* against <root>/data first. Returns NULL when
 * unavailable. */
const char *krkrsdl2_ios_data_root(void);

/* Appends a diagnostic line to Documents/<bundle>/bootstrap.log (the same
 * file the bootstrap page writes; reachable via the Files app). */
void krkrsdl2_ios_log(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* IOS_BOOTSTRAP_H */
