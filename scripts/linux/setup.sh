#!/bin/bash
# scripts/linux/setup.sh
#
# Linux 服务器环境配置与编译
#
# 功能:
#   1. 检查 / 安装依赖 (cmake, g++, socat)
#   2. 检查 TUN 模块
#   3. 下载 ONNX Runtime (如果需要)
#   4. 编译 NeuStack
#
# 用法:
#   sudo bash scripts/linux/setup.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=============================================="
echo "  NeuStack Linux Setup"
echo "=============================================="
echo "  Project: $PROJECT_ROOT"
echo "=============================================="
echo ""

# ─── 1. 检查依赖 ───
echo "[1/4] Checking dependencies..."

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
check_cmd cmake  || MISSING=1
check_cmd g++    || MISSING=1
check_cmd curl   || MISSING=1
check_cmd socat  || MISSING=1

if [ $MISSING -eq 1 ]; then
    echo ""
    echo "  Installing missing packages..."
    if command -v apt-get &> /dev/null; then
        apt-get update -qq
        apt-get install -y -qq cmake g++ curl socat
    elif command -v yum &> /dev/null; then
        yum install -y cmake gcc-c++ curl socat
    elif command -v dnf &> /dev/null; then
        dnf install -y cmake gcc-c++ curl socat
    elif command -v pacman &> /dev/null; then
        pacman -Sy --noconfirm cmake gcc curl socat
    else
        echo "  ERROR: Unknown package manager. Please install: cmake g++ curl socat"
        exit 1
    fi
    echo "  Done"
fi

# 检查 g++ 版本 (需要 C++20 → g++ >= 10)
GCC_VER=$(g++ -dumpversion | cut -d. -f1)
if [ "$GCC_VER" -lt 10 ]; then
    echo ""
    echo "  WARNING: g++ $GCC_VER too old, need >= 10 for C++20"
    echo "  Install newer version:"
    echo "    apt install g++-12"
    echo "    export CXX=g++-12"
    exit 1
fi
echo "  ✓ g++ version $GCC_VER (C++20 OK)"

# ─── 2. TUN 模块 ───
echo ""
echo "[2/4] Checking TUN support..."

if [ -c /dev/net/tun ]; then
    echo "  ✓ /dev/net/tun exists"
else
    echo "  Loading tun module..."
    modprobe tun 2>/dev/null || true
    if [ -c /dev/net/tun ]; then
        echo "  ✓ tun module loaded"
    else
        echo "  ✗ /dev/net/tun not available"
        echo "  Check kernel config: CONFIG_TUN=y or CONFIG_TUN=m"
        exit 1
    fi
fi

# ─── 3. ONNX Runtime ───
echo ""
echo "[3/4] Checking ONNX Runtime..."

ORT_DIR="$PROJECT_ROOT/third_party/onnxruntime/linux-x64"

if [ -f "$ORT_DIR/lib/libonnxruntime.so" ]; then
    echo "  ✓ ONNX Runtime (linux-x64) already exists"
else
    echo "  Downloading ONNX Runtime (linux-x64)..."
    bash "$PROJECT_ROOT/scripts/download_onnxruntime.sh" linux-x64
fi

# ─── 4. 编译 ───
echo ""
echo "[4/4] Building NeuStack..."

BUILD_DIR="$PROJECT_ROOT/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DNEUSTACK_ENABLE_AI=OFF -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo ""
echo "=============================================="
echo "  Build Complete!"
echo "=============================================="
echo ""
echo "  Binary: $BUILD_DIR/neustack"
echo ""
echo "  Next step: start data collection"
echo "    sudo bash scripts/linux/collect.sh"
echo ""
