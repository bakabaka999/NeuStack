#!/bin/bash
# scripts/mac/setup.sh
#
# macOS 环境配置与编译
#
# 功能:
#   1. 检查依赖 (cmake, clang++)
#   2. 下载 ONNX Runtime (如果需要)
#   3. 编译 NeuStack
#
# 用法:
#   bash scripts/mac/setup.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=============================================="
echo "  NeuStack macOS Setup"
echo "=============================================="
echo "  Project: $PROJECT_ROOT"
echo "=============================================="
echo ""

# ─── 1. 检查依赖 ───
echo "[1/3] Checking dependencies..."

check_cmd() {
    if command -v "$1" &> /dev/null; then
        echo "  ✓ $1"
        return 0
    else
        echo "  ✗ $1 (missing)"
        return 1
    fi
}

MISSING=0
check_cmd cmake || MISSING=1
check_cmd clang++ || MISSING=1

if [ $MISSING -eq 1 ]; then
    echo ""
    echo "Please install missing dependencies:"
    echo "  brew install cmake"
    echo "  xcode-select --install"
    exit 1
fi

# ─── 2. ONNX Runtime ───
echo ""
echo "[2/3] Checking ONNX Runtime..."

# 检测架构
if [ "$(uname -m)" = "arm64" ]; then
    ORT_PLATFORM="macos-arm64"
else
    ORT_PLATFORM="macos-x64"
fi

ORT_DIR="$PROJECT_ROOT/third_party/onnxruntime/$ORT_PLATFORM"

if [ -f "$ORT_DIR/lib/libonnxruntime.dylib" ]; then
    echo "  ✓ ONNX Runtime ($ORT_PLATFORM) already exists"
else
    echo "  Downloading ONNX Runtime ($ORT_PLATFORM)..."
    bash "$PROJECT_ROOT/scripts/download_onnxruntime.sh" "$ORT_PLATFORM"
fi

# ─── 3. 编译 ───
echo ""
echo "[3/3] Building NeuStack..."

BUILD_DIR="$PROJECT_ROOT/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DNEUSTACK_ENABLE_AI=OFF
make -j$(sysctl -n hw.ncpu)

echo ""
echo "=============================================="
echo "  Build Complete!"
echo "=============================================="
echo ""
echo "  Binary: $BUILD_DIR/examples/neustack_demo"
echo ""
echo "  Next steps:"
echo "    # 本地测试"
echo "    sudo ./build/examples/neustack_demo --ip 192.168.100.2 -v"
echo ""
echo "    # 带数据采集"
echo "    sudo ./build/examples/neustack_demo --ip 192.168.100.2 --collect -v"
echo ""
echo "    # 配置虚拟网卡 (在另一个终端)"
echo "    sudo ifconfig utunX 192.168.100.1 192.168.100.2 up"
echo ""
