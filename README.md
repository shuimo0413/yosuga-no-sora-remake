# Yosuga no Sora: HD Remake

English | [简体中文](README.zh-CN.md)

**给主群用户：群临时封禁了七天，这七天内别去贴吧接着拱火了，让他们赢麻了这事也就过去了**

2026.9.23 11:04 WarSkyGod留：
我阴阳的从来不是正常玩家，而是那些跟风的串子，该删除的内容我们已经在v1.0.6之后删掉了，那段更新日志里的阴阳怪气是骂那些单纯过来发泄情绪的人，我气的是为什么有人可以对一个免费发布普惠大众的重制版Gal就因为一张免责声明的头图（你们应该知道那图很早以前就有了吧？原图是鸟穹做的，现在他已经跟我们切割了）而大肆辱骂，仿佛我们收了钱还是欠了他们什么东西一样，那张图我个人是无感的，对于这种免责声明类的东西我一向都是看一乐就好的态度，不支持不反对，该叫老婆叫老婆，我自己也一样，我是后加入这个制作组的，所以这张图其实在我加入前就有了，我不是很清楚制作组当时为什么要放这张图，可能是因为视觉风格上与游戏很协调吧，但我没想到有人非得较这个真，如果误伤到正常玩家了我道歉，对不起各位。但这整件事情非常令我寒心，有事情不能好好说话，非得带波节奏拱火来骂，制作组也是人，也会有情绪，本来一件可以正常解决的问题非要阴阳怪气挂到互联网上被人口诛笔伐，我们团队内部自然是有一些怨气在的，也会有一些情绪上的失控，尤其是我们是免费发布的，有人是合理诉求，但更多人只是单纯被带节奏来发泄戾气而已，我个人无意规训任何玩家该怎么玩游戏，我只是单纯不在意这些免责声明类的东西，我那些激进发言也是因为被一堆人骂恼了才发的，望理解。不过互联网这样子也不是一天两天了，唉

对了，纠正一下，我们是高清重置组不是汉化组，很早以前我们曾讨论过是否重新汉化的问题，但得出的结论是没必要重复造轮子，所以汉化文本其实是用了星空网 Sphere 中文化委员会的汉化补丁，我们也在头图中注明了翻译是星空网 Sphere 中文化委员会，也没有更改汉化文本，这是一个很早的补丁了，里面其实有很多错别字，但碍于他们的协议我们没有改动。

This repository contains the complete game project for the Yosuga no Sora HD
remake. The main repository lives at
[shuimo0413/yosuga-no-sora-remake](https://github.com/shuimo0413/yosuga-no-sora-remake).
The cross-platform runtime is the Kirikiri SDL2 engine in `src/`; the Windows
KRKRZ runtime under `platform/` is built from the vendored Kirikiri Z fork in
`external/krkrz` ([LightWinder/krkrz](https://github.com/LightWinder/krkrz), a
fork of [krkrz/krkrz](https://github.com/krkrz/krkrz) carrying the Android
port that produced the first Android release).

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

Run the one-shot bootstrap (submodules + game data):

```sh
git clone --recurse-submodules https://github.com/shuimo0413/yosuga-no-sora-remake.git
cd yosuga-no-sora-remake
./setup.sh            # Windows: setup.bat
```

Game data is not stored in git at all. `setup.sh`
calls `tools/fetch_data_parts.py`, which auto-picks the newest `data-vN`
source release, downloads the multipart zips, verifies every SHA-256, and
extracts them into `data/`.
Re-running the fetch only downloads parts that changed.

### Updating the game data

After editing files under `data/`:

```sh
python tools/publish_data_source.py
```

This detects the changes, packages a new `data-vN` source release, uploads
it via the `gh` CLI (or prints manual upload steps), and commits the
refreshed content manifest — every consumer and CI run then auto-picks the
new release automatically.

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
validates the game data from the source release, regenerates the full content
manifest, packages
the native runtime with `data/`, and creates a GitHub Release automatically.
The workflow can also be started manually with a release tag and prerelease
option.

GitHub limits each release asset to 2 GiB, so the package is published as a
multipart 7-Zip archive. Download every `.7z.NNN` file into the same directory
and open `.7z.001` with 7-Zip. Every release also publishes a
`BUILD-INFO.txt` whose SHA-256 section lists the digest of each archive
volume; there is no separate checksum file.

## Android Releases

The Android release workflow runs for the same `v*` tags and can also be
started manually. It builds only the production ARM64 target with native
`-O3 -DNDEBUG` optimizations and publishes a single data-external APK: the
game data is not embedded, and the in-app bootstrap downloads and imports it
from the GitHub Release (a proxy prefix and a custom download address can be
entered in the bootstrap UI).

For a stable release signature, configure all four repository secrets:
`ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`,
and `ANDROID_KEY_PASSWORD`. Without them, the workflow deliberately uses the
standard Android development key and records that fact in `BUILD-INFO.txt`.

## Apple Releases

The macOS and iOS workflows respond to the same `v*` tags and can also be run
manually. macOS produces an Apple Silicon `.dmg` published as multipart
7-Zip volumes below GitHub's 2 GiB per-asset limit (open the first
`.7z.001` volume to reconstruct it); it embeds the complete `data/`
directory. iOS builds a data-external arm64 `.ipa` published as a single
file: like the Android build, the bootstrap downloads and imports the game
data at first launch.

The macOS app uses ad-hoc signing and is not notarized. The iOS workflow builds
an unsigned IPA by default, suitable for later re-signing. To produce an IPA
that can be installed on devices covered by your provisioning profile, set all
four repository secrets:

- `IOS_CERTIFICATE_P12_BASE64`
- `IOS_CERTIFICATE_PASSWORD`
- `IOS_PROVISIONING_PROFILE_BASE64`
- `IOS_DEVELOPMENT_TEAM`

The default bundle identifier on both platforms is
`com.shuimo0413.yosuganosora.hdremake`. Set the repository variable
`APP_BUNDLE_IDENTIFIER` (the legacy `IOS_BUNDLE_IDENTIFIER` name is still
honoured for iOS) before building if the provisioning profile uses a
different identifier. See `ios-project/README.md` for local Xcode generation.

## OpenHarmony Releases

The OpenHarmony workflow responds to the same `v*` tags and can also be run
manually. It builds an ARM64 HAP for OpenHarmony 5.0 (API 12) on a Linux
runner: the workflow downloads the official OpenHarmony 5.0.0 SDK and command
line tools, patches and builds the vendored SDL2 with the OpenHarmony video
backend (XComponent + EGL), assembles the HAP with Hvigor, and publishes it
as multipart 7-Zip volumes below GitHub's 2 GiB per-asset limit.

The game data ships separately: the HAP is data-external and its bootstrap
downloads the content archives from the GitHub Release at first launch (the
workflow can also build diagnostic bundled/mini variants on manual
dispatch). The workflow publishes an *unsigned* HAP by
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
Every binary release ships a `THIRD-PARTY-NOTICES.txt` (generated by
`tools/generate_notices.py`) that bundles the license texts of all
redistributed components; third-party sources remain subject to the licenses
in their respective directories.
