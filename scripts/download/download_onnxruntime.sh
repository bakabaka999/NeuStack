#!/bin/bash
# download_onnxruntime.sh
# Download pre-built ONNX Runtime libraries
#
# Usage:
#   ./scripts/download_onnxruntime.sh                # auto-detect current platform
#   ./scripts/download_onnxruntime.sh macos-arm64    # specific platform
#   ./scripts/download_onnxruntime.sh all             # all platforms

set -e

VERSION="1.17.0"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEST_DIR="$PROJECT_ROOT/third_party/onnxruntime"

echo "ONNX Runtime Downloader"
echo "======================="
echo "  Version:     $VERSION"
echo "  Destination: $DEST_DIR"
echo ""

mkdir -p "$DEST_DIR"

# ─── Download helper ───
download_platform() {
    local name="$1"
    local url="$2"
    local dest="$DEST_DIR/$name"

    if [ -d "$dest/lib" ]; then
        echo "  ✓ $name already exists, skipping"
        return 0
    fi

    echo "  Downloading $name..."
    local tmp="/tmp/ort-${name}.tgz"
    curl -fSL "$url" -o "$tmp"
    mkdir -p "$dest"
    tar -xzf "$tmp" -C "$dest" --strip-components=1
    rm -f "$tmp"
    echo "  ✓ $name installed"
}

# ─── Platform definitions ───
BASE_URL="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}"

download_macos_arm64() {
    download_platform "macos-arm64" "$BASE_URL/onnxruntime-osx-arm64-${VERSION}.tgz"
}

download_macos_x64() {
    download_platform "macos-x64" "$BASE_URL/onnxruntime-osx-x86_64-${VERSION}.tgz"
}

download_linux_x64() {
    download_platform "linux-x64" "$BASE_URL/onnxruntime-linux-x64-${VERSION}.tgz"
}

download_linux_aarch64() {
    download_platform "linux-aarch64" "$BASE_URL/onnxruntime-linux-aarch64-${VERSION}.tgz"
}

# ─── Auto-detect current platform ───
detect_platform() {
    local os="$(uname -s)"
    local arch="$(uname -m)"

    case "$os" in
        Darwin)
            case "$arch" in
                arm64)  echo "macos-arm64" ;;
                x86_64) echo "macos-x64" ;;
                *)      echo "unknown" ;;
            esac
            ;;
        Linux)
            case "$arch" in
                x86_64)  echo "linux-x64" ;;
                aarch64) echo "linux-aarch64" ;;
                *)       echo "unknown" ;;
            esac
            ;;
        *)
            echo "unknown"
            ;;
    esac
}

# ─── Main ───
PLATFORMS=()

if [ $# -eq 0 ]; then
    # No args → auto-detect
    DETECTED=$(detect_platform)
    if [ "$DETECTED" = "unknown" ]; then
        echo "ERROR: Unsupported platform: $(uname -s) $(uname -m)"
        echo "Supported: macos-arm64, macos-x64, linux-x64, linux-aarch64"
        exit 1
    fi
    echo "  Auto-detected: $DETECTED"
    echo ""
    PLATFORMS+=("$DETECTED")
else
    PLATFORMS=("$@")
fi

for platform in "${PLATFORMS[@]}"; do
    case "$platform" in
        macos-arm64|osx-arm64|arm64)
            download_macos_arm64 ;;
        macos-x64|osx-x64)
            download_macos_x64 ;;
        linux-x64|linux-x86_64)
            download_linux_x64 ;;
        linux-aarch64|linux-arm64)
            download_linux_aarch64 ;;
        all)
            download_macos_arm64
            download_macos_x64
            download_linux_x64
            download_linux_aarch64
            ;;
        *)
            echo "ERROR: Unknown platform '$platform'"
            echo "Supported: macos-arm64, macos-x64, linux-x64, linux-aarch64, all"
            exit 1
            ;;
    esac
done

echo ""
echo "Done! Installed:"
for d in "$DEST_DIR"/*/; do
    [ -d "$d" ] && echo "  $(basename "$d")"
done
