#!/bin/bash
# download_onnxruntime.sh
# 下载 ONNX Runtime 预编译库

set -e

VERSION="1.17.0"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEST_DIR="$PROJECT_ROOT/third_party/onnxruntime"

echo "ONNX Runtime Downloader"
echo "======================="
echo "Version: $VERSION"
echo "Destination: $DEST_DIR"
echo ""

mkdir -p "$DEST_DIR"

# ─────────────────────────────────────────────────────────────────
# macOS ARM64 (Apple Silicon)
# ─────────────────────────────────────────────────────────────────
download_macos_arm64() {
    echo "[1/3] Downloading macOS ARM64..."
    local url="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/onnxruntime-osx-arm64-${VERSION}.tgz"
    local tmp="/tmp/ort-macos-arm64.tgz"

    curl -L "$url" -o "$tmp"
    mkdir -p "$DEST_DIR/macos-arm64"
    tar -xzf "$tmp" -C "$DEST_DIR/macos-arm64" --strip-components=1
    rm "$tmp"
    echo "  Done: $DEST_DIR/macos-arm64"
}

# ─────────────────────────────────────────────────────────────────
# macOS x64 (Intel)
# ─────────────────────────────────────────────────────────────────
download_macos_x64() {
    echo "[2/3] Downloading macOS x64..."
    local url="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/onnxruntime-osx-x86_64-${VERSION}.tgz"
    local tmp="/tmp/ort-macos-x64.tgz"

    curl -L "$url" -o "$tmp"
    mkdir -p "$DEST_DIR/macos-x64"
    tar -xzf "$tmp" -C "$DEST_DIR/macos-x64" --strip-components=1
    rm "$tmp"
    echo "  Done: $DEST_DIR/macos-x64"
}

# ─────────────────────────────────────────────────────────────────
# Linux x64
# ─────────────────────────────────────────────────────────────────
download_linux_x64() {
    echo "[3/3] Downloading Linux x64..."
    local url="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/onnxruntime-linux-x64-${VERSION}.tgz"
    local tmp="/tmp/ort-linux-x64.tgz"

    curl -L "$url" -o "$tmp"
    mkdir -p "$DEST_DIR/linux-x64"
    tar -xzf "$tmp" -C "$DEST_DIR/linux-x64" --strip-components=1
    rm "$tmp"
    echo "  Done: $DEST_DIR/linux-x64"
}

# ─────────────────────────────────────────────────────────────────
# 主逻辑
# ─────────────────────────────────────────────────────────────────

# 解析参数
if [ $# -eq 0 ]; then
    # 无参数：下载所有平台
    download_macos_arm64
    download_macos_x64
    download_linux_x64
else
    # 指定平台
    for platform in "$@"; do
        case "$platform" in
            macos-arm64|osx-arm64|arm64)
                download_macos_arm64
                ;;
            macos-x64|osx-x64|x64)
                download_macos_x64
                ;;
            linux|linux-x64)
                download_linux_x64
                ;;
            all)
                download_macos_arm64
                download_macos_x64
                download_linux_x64
                ;;
            *)
                echo "Unknown platform: $platform"
                echo "Usage: $0 [macos-arm64|macos-x64|linux-x64|all]"
                exit 1
                ;;
        esac
    done
fi

echo ""
echo "Download complete!"
echo ""
echo "Directory structure:"
ls -la "$DEST_DIR"
