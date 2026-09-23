# 缘之空：高清重制

[English](README.md) | 简体中文

**给主群用户：群临时封禁了七天，别去贴吧接着拱火了**

2026.9.23 11:04 WarSkyGod留：
我阴阳的从来不是正常玩家，而是那些跟风的串子，该删除的内容我们已经在v1.0.6之后删掉了，那段更新日志里的阴阳怪气是骂那些单纯过来发泄情绪的人，我气的是为什么有人可以对一个免费发布普惠大众的重制版Gal就因为一张免责声明的头图（你们应该知道那图很早以前就有了吧？原图是鸟穹做的，现在他已经跟我们切割了）而大肆辱骂，仿佛我们收了钱还是欠了他们什么东西一样，那张图我个人是无感的，对于这种免责声明类的东西我一向都是看一乐就好的态度，不支持不反对，该叫老婆叫老婆，我自己也一样，我是后加入这个制作组的，所以这张图其实在我加入前就有了，我不是很清楚制作组当时为什么要放这张图，可能是因为视觉风格上与游戏很协调吧，但我没想到有人非得较这个真，如果误伤到正常玩家了我道歉，对不起各位。但这整件事情非常令我寒心，有事情不能好好说话，非得带波节奏拱火来骂，制作组也是人，也会有情绪，本来一件可以正常解决的问题非要阴阳怪气挂到互联网上被人口诛笔伐，我们团队内部自然是有一些怨气在的，也会有一些情绪上的失控，尤其是我们是免费发布的，有人是合理诉求，但更多人只是单纯被带节奏来发泄戾气而已，我个人无意规训任何玩家该怎么玩游戏，我只是单纯不在意这些免责声明类的东西，我那些激进发言也是因为被一堆人骂恼了才发的，望理解。不过互联网这样子也不是一天两天了，唉

对了，纠正一下，我们是高清重置组不是汉化组，很早以前我们曾讨论过是否重新汉化的问题，但得出的结论是没必要重复造轮子，所以汉化文本其实是用了星空网 Sphere 中文化委员会的汉化补丁，我们也在头图中注明了翻译是星空网 Sphere 中文化委员会，也没有更改汉化文本，这是一个很早的补丁了，里面其实有很多错别字，但碍于他们的协议我们没有改动。

关于所谓“视频下架跑路”的谣言澄清：
那些视频不是被我们主动下架的，而是被人恶意举报导致的下架，很多人都说我们出事了，所以赶紧屁滚尿流的下架跑路了，没有的事情，我们这种非官方无授权的民间自发重置的Gal本来就是灰色地带，我们也知道再申诉也大概率过不了审了

<img width="529" height="450" alt="20aaead60631e9754b261997d2b489ee" src="https://github.com/user-attachments/assets/b2564e81-09ca-409f-af8c-3f5723d9e3fc" />
<img width="854" height="1009" alt="6f1c49be09a09b7a33ee34692a7b6b67_720" src="https://github.com/user-attachments/assets/e9335eea-9fe6-4d76-b57b-b3331e46ada9" />

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

运行一键引导脚本（子模块 + 游戏素材）：

```sh
git clone --recurse-submodules https://github.com/shuimo0413/yosuga-no-sora-remake.git
cd yosuga-no-sora-remake
./setup.sh            # Windows: setup.bat
```

游戏素材完全不存放在 git 仓库中。`setup.sh` 调用
`tools/fetch_data_parts.py`：自动选取编号最大的 `data-vN` 数据源
Release、下载分卷压缩包、逐一校验 SHA-256 并解压到
`data/`。重复执行只会下载有变化的部分。

### 更新游戏素材

修改 `data/` 下的文件后：

```sh
python tools/publish_data_source.py
```

该命令自动检测变化、打包新的 `data-vN` 数据源 Release、通过 `gh` CLI
上传（本机不可用时打印手动上传步骤）并提交更新后的 content-manifest——
所有协作者与 CI 运行随后自动选取最新的数据 Release。

已有工作区更新依赖：

```sh
git submodule sync --recursive
git submodule update --init --recursive
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

推送匹配 `v*` 的标签后，Windows KRKRZ 发布工作流会自动从数据源 Release 校验
游戏素材、重新生成
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
