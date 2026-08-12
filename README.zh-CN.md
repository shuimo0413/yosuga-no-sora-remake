# 缘之空：高清重制

[English](README.md) | 简体中文

本仓库是《缘之空》高清重制的完整游戏工程。项目以修改后的
[Kirikiri SDL2](https://github.com/LightWinder/krkrsdl2) 为跨平台运行基础，
同时保留原生 Kirikiri Z Windows 运行时作为兼容版本。

## 项目结构

- `data/`：唯一的游戏内容源，包含脚本、图片、字体、音频和视频素材。
- `src/`：Kirikiri SDL2 引擎及项目适配源码。
- `external/`：SDL、Kirikiri Z 等固定版本的第三方依赖。
- `android-project/`：Android Gradle 工程，构建时直接使用根目录的 `data/`。
- `platform/windows-krkrz/`：原生 Kirikiri Z Windows 运行时、插件和启动配置。
- `tools/`：内容清单及后续发布工具。

## 获取源码

```sh
git clone --recurse-submodules https://github.com/LightWinder/yosuga-no-sora-remake.git
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
运行时已独立归档。目前已经支持自动打包 Windows KRKRZ 和 Android ARM64。

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
全部 `.7z.NNN` 文件下载到同一目录，然后使用 7-Zip 打开 `.7z.001`。Release 同时提供
SHA-256 校验文件。

## Android 发布

Android 发布工作流会响应同一批 `v*` 标签，也可以手动启动。它只构建采用原生
`-O3 -DNDEBUG`
优化的 ARM64 Release APK，并把超过 GitHub 2 GiB 附件限制的 APK 切成分卷 7-Zip，追加到
与 Windows 包相同的 GitHub Release。请下载全部 `.apk.7z.NNN` 文件，再打开 `.7z.001`
解出 APK。

如需稳定的正式签名，请配置 `ANDROID_KEYSTORE_BASE64`、
`ANDROID_KEYSTORE_PASSWORD`、`ANDROID_KEY_ALIAS` 和 `ANDROID_KEY_PASSWORD` 四个仓库
Secrets。未配置时工作流会明确回退到 Android 开发签名，并在 `BUILD-INFO.txt` 中注明。

Kirikiri SDL2 源码使用 MIT 许可证，详见 `LICENSE`。第三方组件适用各自目录中的许可证。
