# 缘之空：高清重制

[English](README.md) | 简体中文

本仓库是《缘之空》高清重制的完整游戏工程，主仓库位于
[shuimo0413/yosuga-no-sora-remake](https://github.com/shuimo0413/yosuga-no-sora-remake)。
跨平台运行时为 `src/` 中的 Kirikiri SDL2 引擎；`platform/` 下的 Windows KRKRZ
运行时基于 `external/krkrz` 中的 Kirikiri Z 分支构建
（[LightWinder/krkrz](https://github.com/LightWinder/krkrz)，是
[krkrz/krkrz](https://github.com/krkrz/krkrz) 的分支，包含产出首个安卓版本的
Android 移植改动）。

## 项目结构

- `data/`：唯一的游戏内容源，包含脚本、图片、字体、音频和视频素材。
- `src/`：Kirikiri SDL2 引擎及项目适配源码。
- `external/`：SDL、Kirikiri Z 等固定版本的第三方依赖。
- `android-project/`：Android Gradle 工程，构建时直接使用根目录的 `data/`。
- `ios-project/`：通过根目录 CMake 工程生成 iOS Xcode 项目。
- `ohos-project/`：面向 OpenHarmony 5.0（API 12）的 DevEco Studio/Hvigor 工程，
  包含 OpenHarmony SDL2 视频后端与 NAPI 入口模块，详见 `ohos-project/README.md`。
- `platform/windows-krkrz/`：原生 Kirikiri Z Windows 运行时、插件和启动配置。
- `tools/`：内容清单及后续发布工具。

## 获取源码

```sh
git clone --recurse-submodules https://github.com/shuimo0413/yosuga-no-sora-remake.git
cd yosuga-no-sora-remake
git lfs pull
```

已有工作区更新依赖：

```sh
git submodule sync --recursive
git submodule update --init --recursive
git lfs pull
```

## 当前状态

SDL2 桌面端和 Android 工程均从 `data/` 读取游戏内容。Windows KRKRZ
运行时已独立归档。目前已经支持自动打包 Windows KRKRZ、Android ARM64、
Apple Silicon macOS、iOS ARM64 和 OpenHarmony 5.0 ARM64。

## 开发启动

Windows 使用系统自带的 PowerShell 直接运行 KRKRZ，无需安装 Python、编译引擎或
复制素材：

```powershell
.\project.ps1 run windows-krkrz
```

Windows SDL2 版本需要安装 CMake 和 Visual Studio C++ 工具链。第一次需要完整编译，
之后会进行增量编译，同样不需要 Python：

```powershell
.\project.ps1 run windows-sdl2
```

macOS 使用 CMake 创建不包含素材副本的 SDL2 开发构建。第一次需要完整编译，之后
会进行增量编译：

```sh
./project.sh run macos-sdl2
```

需要传递引擎选项时，直接附加到命令末尾：

```sh
./project.sh run macos-sdl2 -about
```

这些启动命令都会直接读取仓库中的 `data/`，修改游戏脚本或素材后无需重新打包。

## Windows KRKRZ 发布

推送匹配 `v*` 的标签后，Windows KRKRZ 发布工作流会自动校验 Git LFS 素材、重新生成
完整内容清单、把原生运行时与 `data/` 打包，并创建 GitHub Release。也可以在 Actions
页面手动输入发布标签，并选择是否标记为预发布版本。

GitHub 要求每个 Release 附件小于 2 GiB，因此游戏包会发布成分卷 7-Zip 压缩包。请把
全部 `.7z.NNN` 文件下载到同一目录，然后使用 7-Zip 打开 `.7z.001`。每个 Release
还会发布 `BUILD-INFO.txt`，其 SHA-256 部分列出各分卷的校验值；不再单独提供校验文件。

## Android 发布

Android 发布工作流会响应同一批 `v*` 标签，也可以手动启动。它只构建采用原生
`-O3 -DNDEBUG`
优化的 ARM64 Release APK，且 APK 不内置游戏数据：引导界面会在首次启动时从
GitHub Release 下载并导入数据（引导 UI 中可填加速代理前缀或自定义下载地址）。

如需稳定的正式签名，请配置 `ANDROID_KEYSTORE_BASE64`、
`ANDROID_KEYSTORE_PASSWORD`、`ANDROID_KEY_ALIAS` 和 `ANDROID_KEY_PASSWORD` 四个仓库
Secrets。未配置时工作流会明确回退到 Android 开发签名，并在 `BUILD-INFO.txt` 中注明。

## Apple 发布

macOS 与 iOS workflow 会响应同一批 `v*` 标签，也可以手动启动。macOS 生成内嵌完整
`data/` 的 Apple Silicon `.dmg`，按 GitHub 单个附件小于 2 GiB 的限制发布成 7-Zip
分卷，打开首个 `.7z.001` 即可还原；iOS 生成数据外置的 arm64 `.ipa`，以单文件发布，
与 Android 一样由引导界面在首次启动时下载并导入游戏数据。

macOS 应用使用 ad-hoc 签名，未做 Apple 公证。iOS 默认生成供后续重签名的 unsigned
IPA。若需生成可安装到 provisioning profile 所覆盖设备上的 IPA，请同时配置四项仓库
Secrets：

- `IOS_CERTIFICATE_P12_BASE64`
- `IOS_CERTIFICATE_PASSWORD`
- `IOS_PROVISIONING_PROFILE_BASE64`
- `IOS_DEVELOPMENT_TEAM`

两个平台的默认 bundle identifier 均为 `com.shuimo0413.yosuganosora.hdremake`。
如果 provisioning profile 使用其他标识，请在构建前设置仓库变量
`APP_BUNDLE_IDENTIFIER`（iOS 侧仍兼容旧的 `IOS_BUNDLE_IDENTIFIER` 变量名）。
本地生成 Xcode 项目的说明见 `ios-project/README.md`。

## OpenHarmony 发布

OpenHarmony 工作流响应同一批 `v*` 标签，也可以手动启动。它在 Linux runner 上为
OpenHarmony 5.0（API 12）构建 ARM64 HAP：下载官方 OpenHarmony 5.0.0 SDK 与命令行
工具，把自带 OpenHarmony 视频后端（XComponent + EGL）的 SDL2 打补丁后编译，用
Hvigor 组装 HAP，并按 GitHub 单个附件小于 2 GiB 的限制发布成 7-Zip 分卷。

游戏数据独立发布：HAP 不内置内容，引导界面在首次启动时从 GitHub Release 下载
数据包（手动触发工作流时也可选择 bundled/mini 诊断变体）。工作流默认发布**未签名**
HAP（sign_mode `none`），安装前必须先用你自己的材料签名：

- **OpenHarmony 设备**：以 sign_mode `community` 重新触发工作流，使用 OpenHarmony
  社区调试证书签名。
- **HarmonyOS 5.0 及以上（NEXT）**：只接受 AppGallery Connect（AGC）颁发的证书与
  Profile。以 sign_mode `agc` 配合六个 `OHOS_*` Secrets 重新触发工作流，或用
  `tools/sign_hap_agc.ps1` 在本地签名下载到的 HAP。完整说明（包括在 AGC 注册
  bundleName 一致的应用）见 `ohos-project/README.md`。已知限制（暂无 SDL 音频后端，
  游戏暂以静音运行）也记录在该文档中。

Kirikiri SDL2 源码使用 MIT 许可证，详见 `LICENSE`。每个二进制 Release 都随附
`THIRD-PARTY-NOTICES.txt`（由 `tools/generate_notices.py` 生成），汇总了所有再分发
组件的许可证文本；第三方源码仍适用各自目录中的许可证。
