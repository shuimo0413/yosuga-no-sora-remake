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
- `ios-project/` generates the iOS Xcode project from the root CMake build.
- `ohos-project/` is the DevEco Studio/Hvigor project for OpenHarmony 5.0
  (API 12), including the OpenHarmony SDL2 video backend and the NAPI entry
  module; see `ohos-project/README.md`.
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
Automated release packaging covers Windows KRKRZ, Android ARM64, Apple Silicon
macOS, iOS ARM64, and OpenHarmony 5.0 ARM64.

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

## Apple Releases

The macOS and iOS workflows respond to the same `v*` tags and can also be run
manually. macOS produces an Apple Silicon `.dmg`; iOS builds an arm64 `.ipa`.
Both packages include the complete `data/` directory and are published as
multipart 7-Zip volumes below GitHub's 2 GiB per-asset limit. Open the first
`.7z.001` volume to reconstruct the DMG or IPA.

The macOS app uses ad-hoc signing and is not notarized. The iOS workflow builds
an unsigned IPA by default, suitable for later re-signing. To produce an IPA
that can be installed on devices covered by your provisioning profile, set all
four repository secrets:

- `IOS_CERTIFICATE_P12_BASE64`
- `IOS_CERTIFICATE_PASSWORD`
- `IOS_PROVISIONING_PROFILE_BASE64`
- `IOS_DEVELOPMENT_TEAM`

The default iOS bundle identifier is
`com.lightwinder.yosuganosora.hdremake`. Set the repository variable
`IOS_BUNDLE_IDENTIFIER` before building if the provisioning profile uses a
different identifier. See `ios-project/README.md` for local Xcode generation.

## OpenHarmony Releases

The OpenHarmony workflow responds to the same `v*` tags and can also be run
manually. It builds an ARM64 HAP for OpenHarmony 5.0 (API 12) on a Linux
runner: the workflow downloads the official OpenHarmony 5.0.0 SDK and command
line tools, patches and builds the vendored SDL2 with the OpenHarmony video
backend (XComponent + EGL), assembles the HAP with Hvigor, and publishes it
as multipart 7-Zip volumes below GitHub's 2 GiB per-asset limit.

The game content is packaged as rawfile and extracted into the application
sandbox on first launch. The workflow publishes an *unsigned* HAP by
default (sign_mode `none`); you must sign it before installing:

- **OpenHarmony devices** - re-run the workflow with sign_mode `community`
  for the community OpenHarmony debug certificate.
- **HarmonyOS 5.0+ (NEXT)** - only AppGallery Connect issued certificates
  and profiles are accepted. Re-run the workflow with sign_mode `agc` and
  the six `OHOS_*` repository secrets, or sign the downloaded HAP locally
  with `tools/sign_hap_agc.ps1`. Full instructions (including registering
  the app in AGC with the matching bundle name) are in
  `ohos-project/README.md`. Known limitations (no SDL audio backend yet,
  so the game runs without sound) are listed there as well.

The Kirikiri SDL2 source code is licensed under the MIT License; see `LICENSE`.
Third-party components remain subject to the licenses in their respective
directories.
