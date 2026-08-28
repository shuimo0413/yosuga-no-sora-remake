#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="${IOS_BUILD_DIR:-$PROJECT_ROOT/build/ios}"
CONFIGURATION="${IOS_CONFIGURATION:-Release}"
VERSION_NAME="${IOS_VERSION_NAME:-0.0.0}"
BUILD_NUMBER="${IOS_BUILD_NUMBER:-1}"
BUNDLE_IDENTIFIER="${IOS_BUNDLE_IDENTIFIER:-com.shuimo0413.yosuganosora.hdremake}"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-15.0}"
CODE_SIGN_IDENTITY="${IOS_CODE_SIGN_IDENTITY:-}"
DEVELOPMENT_TEAM="${IOS_DEVELOPMENT_TEAM:-}"
PROVISIONING_PROFILE="${IOS_PROVISIONING_PROFILE:-}"
EMBED_DATA="${IOS_EMBED_DATA:-ON}"

# Preserve a Development Team selected in Xcode when regenerating the CMake
# project. An explicitly provided environment value (including CI secrets)
# always takes precedence.
EXISTING_XCODE_PROJECT="$BUILD_DIR/krkrsdl2.xcodeproj/project.pbxproj"
if [[ -z "$DEVELOPMENT_TEAM" && -f "$EXISTING_XCODE_PROJECT" ]]; then
	DEVELOPMENT_TEAM="$(sed -nE \
		's/.*DEVELOPMENT_TEAM = "?([^";]+)"?;.*/\1/p' \
		"$EXISTING_XCODE_PROJECT" | head -n 1)"
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
	echo "The iOS project can only be generated on macOS." >&2
	exit 1
fi

# Prefer an explicitly selected Xcode, then a regular installation, then the
# beta installation used by local development. This avoids requiring a global
# xcode-select change when it still points at CommandLineTools.
if [[ -n "${DEVELOPER_DIR:-}" && -x "$DEVELOPER_DIR/usr/bin/xcodebuild" ]]; then
	:
elif [[ -x /Applications/Xcode.app/Contents/Developer/usr/bin/xcodebuild ]]; then
	export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
elif [[ -x /Applications/Xcode-beta.app/Contents/Developer/usr/bin/xcodebuild ]]; then
	export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer
fi

if ! command -v xcodebuild >/dev/null 2>&1 ||
	! xcodebuild -version >/dev/null 2>&1; then
	echo "A full Xcode installation is required to generate the iOS project." >&2
	exit 1
fi
SELECTED_DEVELOPER_DIR="${DEVELOPER_DIR:-$(xcode-select -p)}"

# Use CMake from PATH when available. The final fallback is the local bundled
# CMake currently used by this workspace, so a separate system installation is
# not required on the development Mac.
if command -v cmake >/dev/null 2>&1; then
	CMAKE_BIN="$(command -v cmake)"
else
	LOCAL_CMAKE="$PROJECT_ROOT/../krkrsdl2/.codex-tools/cmake-venv/lib/python3.9/site-packages/cmake/data/bin/cmake"
	if [[ -x "$LOCAL_CMAKE" ]]; then
		CMAKE_BIN="$LOCAL_CMAKE"
		export PATH="$(dirname -- "$CMAKE_BIN"):$PATH"
	else
		CMAKE_BIN=""
	fi
fi

if [[ -z "$CMAKE_BIN" ]]; then
	echo "CMake 3.24 or newer is required to generate the iOS project." >&2
	exit 1
fi

if [[ "$EMBED_DATA" != "OFF" && ! -f "$PROJECT_ROOT/data/startup.tjs" ]]; then
	echo "Game data is incomplete: data/startup.tjs was not found." >&2
	echo "For externalized-data builds set IOS_EMBED_DATA=OFF." >&2
	exit 1
fi

if [[ ! "$VERSION_NAME" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
	echo "IOS_VERSION_NAME must contain three numeric components (for example 1.2.3)." >&2
	exit 1
fi
if [[ ! "$BUILD_NUMBER" =~ ^[1-9][0-9]*$ ]]; then
	echo "IOS_BUILD_NUMBER must be a positive integer." >&2
	exit 1
fi
if [[ ! "$BUNDLE_IDENTIFIER" =~ ^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$ ]]; then
	echo "IOS_BUNDLE_IDENTIFIER is invalid: $BUNDLE_IDENTIFIER" >&2
	exit 1
fi

echo "Using Xcode: $SELECTED_DEVELOPER_DIR"
echo "Using CMake: $CMAKE_BIN"
if [[ -n "$DEVELOPMENT_TEAM" ]]; then
	echo "Using Apple Development Team: $DEVELOPMENT_TEAM"
else
	echo "Apple Development Team: select one in Xcode for device builds"
fi

"$CMAKE_BIN" \
	-S "$PROJECT_ROOT" \
	-B "$BUILD_DIR" \
	-G Xcode \
	-DCMAKE_SYSTEM_NAME=iOS \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
	-DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
	-DKRKRSDL2_BUNDLE_IDENTIFIER="$BUNDLE_IDENTIFIER" \
	-DKRKRSDL2_VERSION_NAME="$VERSION_NAME" \
	-DKRKRSDL2_BUILD_NUMBER="$BUILD_NUMBER" \
	-DKRKRSDL2_IOS_CODE_SIGN_IDENTITY="$CODE_SIGN_IDENTITY" \
	-DKRKRSDL2_IOS_DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" \
	-DKRKRSDL2_IOS_PROVISIONING_PROFILE="$PROVISIONING_PROFILE" \
	-DOPTION_EMBED_IOS_DATA="$EMBED_DATA" \
	-DKRKRSDL2_GENERATE_CONTENT_MANIFEST=ON \
	-DOPTION_ENABLE_EXTERNAL_PLUGINS=OFF

echo "Generated: $BUILD_DIR/krkrsdl2.xcodeproj"
echo "Build without launching:"
echo "  '$CMAKE_BIN' --build '$BUILD_DIR' --config '$CONFIGURATION' --target krkrsdl2 --parallel"
echo "To run on a device, open the project in Xcode and select your Development Team."
