# iOS project

The iOS application is an SDL2/UIKit target generated from the repository's
root CMake project. Generated Xcode files contain machine-specific absolute
paths, so they are intentionally kept out of Git and reproduced on demand.

Requirements:

- macOS with a full Xcode installation
- CMake 3.24 or newer
- Git LFS assets downloaded with `git lfs pull`
- Xcode with an iOS 15 or newer device SDK

Generate the Xcode project:

```sh
./ios-project/generate.sh
open build/ios/krkrsdl2.xcodeproj
```

Choose the `krkrsdl2` target and your Apple Development Team before installing
on a device. Local device builds use Xcode's automatic signing, and the
generator preserves the selected Team when the project is regenerated. The
generated project embeds `data/` inside the application bundle
(IOS_EMBED_DATA=OFF disables that) and targets iPhone and iPad in landscape
orientation.

## Data externalization

Release builds (and local builds with `IOS_EMBED_DATA=OFF`) ship WITHOUT the
game data. On first launch a bootstrap page (mirroring the OpenHarmony shell
page) shows over the engine: it downloads the data zips from the GitHub
Release (data-assets.json, with a user-editable download URL and the three
proxy presets 直连 / gh-proxy / Craft-Hello Proxy) or imports a local zip /
data.xp3, extracts everything into Documents/<bundle>/data and then starts
the game. The screen stays awake during download/import/extraction, the
status bar is hidden and the home indicator auto-hides.

The first application installed with a Personal Team may require explicit
approval on the iPhone under **Settings > General > VPN & Device Management**.
This is an iOS trust prompt rather than a build or code-signing failure. Ensure
Developer Mode is also enabled on development devices.

The generator never starts an app, simulator, or audio device. A command-line
build can therefore be checked without playing sound:

```sh
cmake --build build/ios --config Release --target krkrsdl2 --parallel
```

The GitHub iOS release workflow overrides the local automatic-signing default.
It can optionally sign the app when all documented Apple signing secrets are
configured. Without them, it explicitly disables signing and emits an unsigned
IPA that must be re-signed before installation on a physical device.

The signing secrets are `IOS_CERTIFICATE_P12_BASE64`,
`IOS_CERTIFICATE_PASSWORD`, `IOS_PROVISIONING_PROFILE_BASE64`, and
`IOS_DEVELOPMENT_TEAM`. The certificate and mobile provisioning profile values
must be base64 encoded. If the profile does not use the default
`com.lightwinder.yosuganosora.hdremake` identifier, set the repository variable
`IOS_BUNDLE_IDENTIFIER` as well.
