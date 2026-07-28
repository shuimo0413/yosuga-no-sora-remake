#!/bin/zsh

# Double-click launcher for the native macOS krkrsdl2 runtime.
# The runtime may be bundled beside this script or kept in ~/Downloads.

set -u

PROJECT_DIR="${0:A:h}"
LOCAL_RUNTIME="$PROJECT_DIR/krkrsdl2"
DOWNLOAD_RUNTIME="$HOME/Downloads/krkrsdl2"
LOG_DIR="$PROJECT_DIR/savedata"
LAUNCH_LOG="$LOG_DIR/mac-launch.log"
MAC_FONT_SOURCE="/System/Library/Fonts/Supplemental/AppleGothic.ttf"
MAC_FONT_LINK="$PROJECT_DIR/content-data/font-macos.ttc"

cd "$PROJECT_DIR" || exit 1
mkdir -p "$LOG_DIR"

if [[ -x "$LOCAL_RUNTIME" ]]; then
    RUNTIME="$LOCAL_RUNTIME"
elif [[ -x "$DOWNLOAD_RUNTIME" ]]; then
    echo "首次运行：正在把 krkrsdl2 安装到项目目录..."
    cp -p "$DOWNLOAD_RUNTIME" "$LOCAL_RUNTIME"
    RUNTIME="$LOCAL_RUNTIME"
else
    echo "找不到 krkrsdl2。"
    echo "请把它放到以下任一位置："
    echo "  $LOCAL_RUNTIME"
    echo "  $DOWNLOAD_RUNTIME"
    echo
    read -k 1 "?按任意键关闭..."
    exit 1
fi

# Safari downloads may carry Gatekeeper quarantine metadata. Removing only
# this attribute avoids the misleading double-click-and-disappear behaviour.
xattr -d com.apple.quarantine "$RUNTIME" 2>/dev/null || true
chmod +x "$RUNTIME"

# This krkrsdl2 build uses FreeType but does not enumerate native macOS fonts.
# Expose one system CJK font inside the project; Initialize.tjs registers it.
if [[ -f "$MAC_FONT_SOURCE" ]]; then
    ln -sfn "$MAC_FONT_SOURCE" "$MAC_FONT_LINK"
fi

# Ask SDL/Cocoa for a Retina backing surface when the runtime supports it.
# This also overrides any inherited shell setting that disables HiDPI.
export SDL_VIDEO_HIGHDPI_DISABLED=0

echo "[$(date '+%Y-%m-%d %H:%M:%S')] starting $RUNTIME" | tee -a "$LAUNCH_LOG"
# The local runtime deliberately sits beside this project's content-data.
# krkrsdl2 then selects content-data as its project directory automatically.
"$RUNTIME" "$@" 2>&1 | tee -a "$LAUNCH_LOG"
STATUS=${pipestatus[1]}

echo "[$(date '+%Y-%m-%d %H:%M:%S')] exited with status $STATUS" | tee -a "$LAUNCH_LOG"
echo "KiriKiri 日志：$HOME/Library/Application Support/krkrsdl2/krkr.console.log"
echo "启动器日志：$LAUNCH_LOG"

if (( STATUS != 0 )); then
    echo
    read -k 1 "?运行异常，按任意键关闭..."
fi

exit "$STATUS"
