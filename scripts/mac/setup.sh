#!/bin/bash
# scripts/mac/setup.sh
#
# macOS environment setup & build
#
# What it does:
#   1. Check dependencies (cmake, clang++, ninja)
#   2. Download ONNX Runtime if needed
#   3. Build NeuStack (auto-enable AI if ORT found)
#
# Usage:
#   bash scripts/mac/setup.sh [--no-ai]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Parse args
FORCE_NO_AI=0
for arg in "$@"; do
    case "$arg" in
        --no-ai) FORCE_NO_AI=1 ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--no-ai]"; exit 1 ;;
    esac
done

echo "=============================================="
echo "  NeuStack macOS Setup"
echo "=============================================="
echo "  Project: $PROJECT_ROOT"
echo "=============================================="
echo ""

# ─── 1. Check dependencies ───
echo "[1/3] Checking dependencies..."

check_cmd() {
    if command -v "$1" &> /dev/null; then
        echo "  ✓ $1 ($(command -v "$1"))"
        return 0
    else
        echo "  ✗ $1 (missing)"
        return 1
    fi
}

MISSING=0
check_cmd cmake   || MISSING=1
check_cmd clang++ || MISSING=1

# Ninja is preferred but not required
HAS_NINJA=0
if command -v ninja &> /dev/null; then
    echo "  ✓ ninja ($(command -v ninja))"
    HAS_NINJA=1
else
    echo "  - ninja (not found, will use make)"
fi

if [ $MISSING -eq 1 ]; then
    echo ""
    echo "  Please install missing dependencies:"
    echo "    brew install cmake ninja"
    echo "    xcode-select --install"
    exit 1
fi

# Check clang++ version (need C++20 → Apple Clang >= 14 or LLVM Clang >= 14)
CLANG_VER=$(clang++ --version | head -1)
echo "  Compiler: $CLANG_VER"

# ─── 2. ONNX Runtime ───
echo ""
echo "[2/3] Checking ONNX Runtime..."

# Detect architecture
ARCH="$(uname -m)"
if [ "$ARCH" = "arm64" ]; then
    ORT_PLATFORM="macos-arm64"
else
    ORT_PLATFORM="macos-x64"
fi
echo "  Architecture: $ARCH → $ORT_PLATFORM"

ORT_DIR="$PROJECT_ROOT/third_party/onnxruntime/$ORT_PLATFORM"

if [ -f "$ORT_DIR/lib/libonnxruntime.dylib" ]; then
    echo "  ✓ ONNX Runtime already installed"
    ORT_FOUND=1
else
    echo "  Downloading ONNX Runtime ($ORT_PLATFORM)..."
    if bash "$PROJECT_ROOT/scripts/download_onnxruntime.sh" "$ORT_PLATFORM"; then
        ORT_FOUND=1
        echo "  ✓ ONNX Runtime downloaded"
    else
        ORT_FOUND=0
        echo "  ✗ Download failed, AI features will be disabled"
    fi
fi

# Determine AI flag
if [ $FORCE_NO_AI -eq 1 ]; then
    ENABLE_AI=OFF
    echo "  AI: disabled (--no-ai flag)"
elif [ $ORT_FOUND -eq 1 ]; then
    ENABLE_AI=ON
    echo "  AI: enabled (ONNX Runtime found)"
else
    ENABLE_AI=OFF
    echo "  AI: disabled (ONNX Runtime not available)"
fi

# ─── 3. Build ───
echo ""
echo "[3/3] Building NeuStack..."

BUILD_DIR="$PROJECT_ROOT/build"
mkdir -p "$BUILD_DIR"

# Select generator
CMAKE_ARGS=(
    -DNEUSTACK_ENABLE_AI=$ENABLE_AI
    -DCMAKE_BUILD_TYPE=Release
)
if [ $HAS_NINJA -eq 1 ]; then
    CMAKE_ARGS+=(-G Ninja)
    BUILD_CMD="ninja"
else
    BUILD_CMD="make -j$(sysctl -n hw.ncpu)"
fi

cd "$BUILD_DIR"
cmake .. "${CMAKE_ARGS[@]}"
$BUILD_CMD

echo ""
echo "=============================================="
echo "  ✓ Build Complete!"
echo "=============================================="
echo ""
echo "  AI enabled:  $ENABLE_AI"
echo "  Binary:      $BUILD_DIR/examples/neustack_demo"
echo ""
echo "  Quick start:"
echo "    # Start the stack (requires root for utun)"
echo "    sudo ./build/examples/neustack_demo --ip 192.168.100.2 -v"
echo ""
echo "    # Configure NAT (in another terminal)"
echo "    sudo ./scripts/nat/setup_nat.sh --dev utun4"
echo ""
echo "    # With data collection"
echo "    sudo ./build/examples/neustack_demo --ip 192.168.100.2 --collect -v"
echo ""
