#!/bin/bash
# scripts/download_wintun.sh
# Download Wintun DLL for Windows
#
# Usage:
#   ./scripts/download_wintun.sh           # downloads to project root
#   ./scripts/download_wintun.sh ./build   # downloads to specified dir
#
# Wintun is a layer-3 TUN driver by Jason A. Donenfeld (WireGuard project).
# License: https://www.wintun.net/ (prebuilt, free to redistribute)

set -e

VERSION="0.14.1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DEST_DIR="${1:-$PROJECT_ROOT}"

URL="https://www.wintun.net/builds/wintun-${VERSION}.zip"
TMP="/tmp/wintun-${VERSION}.zip"

echo "Wintun Downloader"
echo "================="
echo "  Version:     $VERSION"
echo "  Destination: $DEST_DIR"
echo ""

# Check if already exists
if [ -f "$DEST_DIR/wintun.dll" ]; then
    echo "  ✓ wintun.dll already exists, skipping"
    exit 0
fi

echo "  Downloading..."
curl -fSL "$URL" -o "$TMP"

echo "  Extracting (amd64)..."
# Wintun zip structure: wintun/bin/amd64/wintun.dll
# Also has arm64: wintun/bin/arm64/wintun.dll

# Detect architecture
ARCH="${PROCESSOR_ARCHITECTURE:-AMD64}"
case "$ARCH" in
    AMD64|x86_64)  WINTUN_ARCH="amd64" ;;
    ARM64|aarch64) WINTUN_ARCH="arm64" ;;
    *)             WINTUN_ARCH="amd64" ;;
esac

# Extract with Python (more portable than unzip on Windows)
python3 -c "
import zipfile, sys, shutil, os
with zipfile.ZipFile('$TMP') as z:
    src = f'wintun/bin/$WINTUN_ARCH/wintun.dll'
    if src not in z.namelist():
        print(f'ERROR: {src} not found in archive')
        sys.exit(1)
    with z.open(src) as f_in:
        dst = os.path.join('$DEST_DIR', 'wintun.dll')
        with open(dst, 'wb') as f_out:
            shutil.copyfileobj(f_in, f_out)
        print(f'  ✓ Extracted to {dst}')
" 2>/dev/null || {
    # Fallback: try unzip
    mkdir -p /tmp/wintun-extract
    unzip -q "$TMP" -d /tmp/wintun-extract
    cp "/tmp/wintun-extract/wintun/bin/$WINTUN_ARCH/wintun.dll" "$DEST_DIR/"
    rm -rf /tmp/wintun-extract
    echo "  ✓ Extracted to $DEST_DIR/wintun.dll"
}

rm -f "$TMP"
echo ""
echo "Done! Place wintun.dll alongside neustack_demo.exe to run."
