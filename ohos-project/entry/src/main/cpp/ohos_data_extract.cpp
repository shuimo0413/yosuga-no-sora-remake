/* SPDX-License-Identifier: MIT */
/*
 * Copies the packaged rawfile game content ("data/**") into the application
 * sandbox ({filesDir}/data) where the Kirikiri engine reads it as a normal
 * filesystem tree.
 *
 * Extraction is skipped when the extracted tree matches the packaged
 * content-manifest.json; the hash of that file is stored inside the sandbox.
 */

#include "ohos_data_extract.h"
#include "krkrsdl2_ohos_entry.h"
#include "sdl_ohos_bridge.h"

#include <hilog/log.h>
#include <rawfile/raw_dir.h>
#include <rawfile/raw_file.h>
#include <rawfile/raw_file_manager.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static NativeResourceManager *GetResourceManager()
{
	return static_cast<NativeResourceManager *>(OHOS_Entry_GetResourceManager());
}

static bool MakeDirectoryRecursive(const std::string &path)
{
	if (path.empty())
	{
		return true;
	}
	struct stat st;
	if (stat(path.c_str(), &st) == 0)
	{
		return S_ISDIR(st.st_mode);
	}
	size_t pos = path.rfind('/');
	if (pos != std::string::npos && pos > 0)
	{
		if (!MakeDirectoryRecursive(path.substr(0, pos)))
		{
			return false;
		}
	}
	return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

static std::string JoinPath(const std::string &base, const std::string &name)
{
	if (base.empty() || base == "/")
	{
		return "/" + name;
	}
	return base + "/" + name;
}

static bool ReadRawFileFully(NativeResourceManager *manager, const std::string &raw_path, std::string &out)
{
	out.clear();
	RawFile *file = OH_ResourceManager_OpenRawFile(manager, raw_path.c_str());
	if (file == nullptr)
	{
		return false;
	}
	long size = OH_ResourceManager_GetRawFileSize(file);
	if (size <= 0)
	{
		OH_ResourceManager_CloseRawFile(file);
		return true;
	}
	out.resize(static_cast<size_t>(size));
	long total = 0;
	while (total < size)
	{
		long read = OH_ResourceManager_ReadRawFile(
			file, &out[static_cast<size_t>(total)], static_cast<size_t>(size - total));
		if (read <= 0)
		{
			break;
		}
		total += read;
	}
	OH_ResourceManager_CloseRawFile(file);
	return total == size;
}

static bool WriteFileAll(const std::string &path, const char *data, size_t size)
{
	FILE *file = fopen(path.c_str(), "wb");
	if (file == nullptr)
	{
		return false;
	}
	size_t written = fwrite(data, 1, size, file);
	fclose(file);
	return written == size;
}

static bool ReadFileAll(const std::string &path, std::string &out)
{
	out.clear();
	FILE *file = fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		return false;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0)
	{
		fclose(file);
		return true;
	}
	out.resize(static_cast<size_t>(size));
	size_t read = fread(&out[0], 1, static_cast<size_t>(size), file);
	fclose(file);
	return read == static_cast<size_t>(size);
}

static unsigned long long Fnv1a(const char *data, size_t size)
{
	unsigned long long hash = 1469598103934665603ULL;
	for (size_t i = 0; i < size; i++)
	{
		hash ^= static_cast<unsigned char>(data[i]);
		hash *= 1099511628211ULL;
	}
	return hash;
}

static bool ExtractFile(NativeResourceManager *manager,
	const std::string &raw_path, const std::string &dest_path)
{
	RawFile *file = OH_ResourceManager_OpenRawFile(manager, raw_path.c_str());
	if (file == nullptr)
	{
		return false;
	}
	long size = OH_ResourceManager_GetRawFileSize(file);
	bool ok = true;
	FILE *out = fopen(dest_path.c_str(), "wb");
	if (out == nullptr)
	{
		ok = false;
	}
	else
	{
		char buffer[64 * 1024];
		long remaining = size;
		while (remaining > 0)
		{
			long chunk = remaining < static_cast<long>(sizeof(buffer))
				? remaining : static_cast<long>(sizeof(buffer));
			long read = OH_ResourceManager_ReadRawFile(file, buffer, static_cast<size_t>(chunk));
			if (read <= 0)
			{
				ok = false;
				break;
			}
			if (fwrite(buffer, 1, static_cast<size_t>(read), out) != static_cast<size_t>(read))
			{
				ok = false;
				break;
			}
			remaining -= read;
		}
		fclose(out);
	}
	OH_ResourceManager_CloseRawFile(file);
	return ok;
}

static bool ExtractDirectory(NativeResourceManager *manager,
	const std::string &raw_dir, const std::string &dest_dir)
{
	RawDir *dir = OH_ResourceManager_OpenRawDir(manager, raw_dir.c_str());
	if (dir == nullptr)
	{
		return false;
	}

	if (!MakeDirectoryRecursive(dest_dir))
	{
		OH_ResourceManager_CloseRawDir(dir);
		return false;
	}

	int count = OH_ResourceManager_GetRawFileCount(dir);
	for (int i = 0; i < count; i++)
	{
		const char *name = OH_ResourceManager_GetRawFileName(dir, i);
		if (name == nullptr)
		{
			continue;
		}
		std::string raw_path = JoinPath(raw_dir, name);
		std::string dest_path = JoinPath(dest_dir, name);

		RawFile *file = OH_ResourceManager_OpenRawFile(manager, raw_path.c_str());
		if (file != nullptr)
		{
			OH_ResourceManager_CloseRawFile(file);
			if (!ExtractFile(manager, raw_path, dest_path))
			{
			}
		}
		else
		{
			// Not a file: recurse into the subdirectory.
			ExtractDirectory(manager, raw_path, dest_path);
		}
	}
	OH_ResourceManager_CloseRawDir(dir);
	return true;
}

int OHOS_ExtractGameData(void)
{
	NativeResourceManager *manager = GetResourceManager();
	const char *files_dir = SDL_OHOS_GetFilesDir();
	if (manager == nullptr || files_dir == nullptr)
	{
		return 0;
	}

	const std::string dest_root = JoinPath(std::string(files_dir), "data");
	const std::string hash_file = JoinPath(dest_root, ".ohos_extract_manifest_hash");

	// Skip the whole extraction when the extracted tree already matches the
	// packaged content manifest. This keeps the 3+ GiB copy a first-run cost.
	std::string raw_manifest;
	std::string extracted_manifest;
	bool manifest_available = ReadRawFileFully(manager, "data/content-manifest.json", raw_manifest);
	if (manifest_available)
	{
		std::string previous_hash;
		ReadFileAll(hash_file, previous_hash);
		char hash_text[32];
		snprintf(hash_text, sizeof(hash_text), "%llx", Fnv1a(raw_manifest.data(), raw_manifest.size()));
		if (previous_hash == hash_text &&
			ReadFileAll(JoinPath(dest_root, "content-manifest.json"), extracted_manifest) &&
			extracted_manifest == raw_manifest)
		{
			return 1;
		}
	}
	else
	{
	}

	if (!ExtractDirectory(manager, "data", dest_root))
	{
		return 0;
	}

	if (manifest_available)
	{
		char hash_text[32];
		snprintf(hash_text, sizeof(hash_text), "%llx", Fnv1a(raw_manifest.data(), raw_manifest.size()));
		WriteFileAll(hash_file, hash_text, strlen(hash_text));
	}
	return 1;
}
