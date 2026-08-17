/* SPDX-License-Identifier: MIT */
/*
 * Filesystem hooks for the OpenHarmony SDL2 backend.
 *
 * SDL_GetBasePath returns the application sandbox files directory (where the
 * game data is extracted) and SDL_GetPrefPath returns a savedata directory
 * inside it. Both paths come from the NAPI entry through sdl_ohos_bridge.h.
 */

#include "../../SDL_internal.h"

#include <sys/stat.h>

#include "sdl_ohos_bridge.h"

static char *SDL_OHOS_PathForBase(const char *subdir)
{
	/* The external base directory (public Download app folder) takes
	 * precedence over the sandbox files directory. */
	const char *base = SDL_OHOS_GetDataDir();
	size_t base_length;
	size_t sub_length;
	size_t total;
	char *path;

	if (base == NULL || base[0] == '\0')
	{
		base = SDL_OHOS_GetFilesDir();
	}
	if (base == NULL || base[0] == '\0')
	{
		base = "/";
	}
	base_length = SDL_strlen(base);
	sub_length = subdir != NULL ? SDL_strlen(subdir) : 0;

	/* base + '/' + subdir + '/' + NUL */
	total = base_length + 1 + sub_length + 1 + 1;
	path = (char *)SDL_malloc(total);
	if (path == NULL)
	{
		return NULL;
	}
	SDL_snprintf(path, total, "%s/%s/", base, subdir != NULL ? subdir : "");
	return path;
}

static void SDL_OHOS_MakeDirectoryRecursive(char *path)
{
	char *cursor;

	if (path == NULL || path[0] == '\0')
	{
		return;
	}
	for (cursor = path + 1; *cursor != '\0'; cursor++)
	{
		if (*cursor != '/')
		{
			continue;
		}
		*cursor = '\0';
		mkdir(path, 0700);
		*cursor = '/';
	}
}

char *SDL_GetBasePath(void)
{
	return SDL_OHOS_PathForBase(NULL);
}

char *SDL_GetPrefPath(const char *org, const char *app)
{
	(void)org;
	/* The external savedata directory wins when set, so save files stay
	 * user-accessible in the public Download app folder. */
	const char *save_dir = SDL_OHOS_GetSaveDir();
	if (save_dir != NULL && save_dir[0] != '\0')
	{
		size_t length = SDL_strlen(save_dir);
		char *path = (char *)SDL_malloc(length + 2);
		if (path == NULL)
		{
			return NULL;
		}
		SDL_snprintf(path, length + 2, "%s/", save_dir);
		SDL_OHOS_MakeDirectoryRecursive(path);
		return path;
	}

	const char *leaf = app != NULL ? app : "krkrsdl2";
	char *path = SDL_OHOS_PathForBase(leaf);

	if (path != NULL)
	{
		SDL_OHOS_MakeDirectoryRecursive(path);
	}
	return path;
}
