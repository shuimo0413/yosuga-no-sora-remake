# Yosuga no Sora: HD Remake

English | [简体中文](README.zh-CN.md)

This repository contains the complete game project for the Yosuga no Sora HD
remake. It uses a modified version of
[Kirikiri SDL2](https://github.com/LightWinder/krkrsdl2) as its cross-platform
runtime while retaining the native Kirikiri Z runtime as a Windows-compatible
alternative.

## Project Structure

- `data/` is the single source of game content, including scripts, images,
  fonts, audio, and video assets.
- `src/` contains the Kirikiri SDL2 engine and project-specific adaptations.
- `external/` contains pinned third-party dependencies such as SDL and
  Kirikiri Z.
- `android-project/` is the Android Gradle project and reads content directly
  from the root `data/` directory while building.
- `platform/windows-krkrz/` contains the native Kirikiri Z Windows runtime,
  plugins, and startup configuration.
- `tools/` contains content-manifest utilities and future release tooling.

## Getting the Source

Clone the repository with its submodules, then download the Git LFS content:

```sh
git clone --recurse-submodules https://github.com/LightWinder/yosuga-no-sora-remake.git
cd yosuga-no-sora-remake
git lfs pull
```

To update the dependencies in an existing working tree:

```sh
git submodule sync --recursive
git submodule update --init --recursive
git lfs pull
```

## Current Status

The SDL2 desktop targets and Android project both read game content from
`data/`. The Windows KRKRZ runtime is stored separately under `platform/`.
Automated release packaging currently covers Windows KRKRZ and Android ARM64.

## Development Launchers

### Windows KRKRZ

Windows can launch the prebuilt native KRKRZ runtime using the built-in
PowerShell. This requires neither Python nor an engine build and does not copy
game assets:

```powershell
.\project.ps1 run windows-krkrz
```

### Windows SDL2

The Windows SDL2 version requires CMake and a Visual Studio C++ toolchain. The
first launch performs a full build; subsequent launches build incrementally.
Python is not required:

```powershell
.\project.ps1 run windows-sdl2
```

### macOS SDL2

On macOS, CMake creates an SDL2 development build without embedding a copy of
the game assets. The first launch performs a full build; subsequent launches
build incrementally:

```sh
./project.sh run macos-sdl2
```

Append engine options directly to a launcher command when needed:

```sh
./project.sh run macos-sdl2 -about
```

All development launchers read the repository's `data/` directory directly,
so changes to game scripts and assets do not require repackaging.

## Windows KRKRZ Releases

Pushing a tag matching `v*` runs the Windows KRKRZ release workflow. It
validates the Git LFS content, regenerates the full content manifest, packages
the native runtime with `data/`, and creates a GitHub Release automatically.
The workflow can also be started manually with a release tag and prerelease
option.

GitHub limits each release asset to 2 GiB, so the package is published as a
multipart 7-Zip archive. Download every `.7z.NNN` file into the same directory
and open `.7z.001` with 7-Zip. A SHA-256 checksum file is included with the
release assets.

## Android Releases

The Android release workflow runs for the same `v*` tags and can also be
started manually. It builds only the production ARM64 target with native
`-O3 -DNDEBUG` optimizations, packages the APK as multipart 7-Zip volumes below
GitHub's 2 GiB asset limit, and adds them to the same GitHub Release as the
Windows package. Download every `.apk.7z.NNN` file, then open `.7z.001` to
extract the APK.

For a stable release signature, configure all four repository secrets:
`ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`,
and `ANDROID_KEY_PASSWORD`. Without them, the workflow deliberately uses the
standard Android development key and records that fact in `BUILD-INFO.txt`.

The Kirikiri SDL2 source code is licensed under the MIT License; see `LICENSE`.
Third-party components remain subject to the licenses in their respective
directories.
