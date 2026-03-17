#!/bin/bash
# scripts/linux/setup.sh
#
# Linux environment setup & build
#
# What it does:
#   1. Check / install dependencies (cmake, g++, ninja, socat)
#   2. Check TUN module
#   3. Download ONNX Runtime if needed (auto-detect arch)
#   4. Build NeuStack (auto-enable AI if ORT found)
#
# Usage:
#   sudo bash scripts/linux/setup.sh [--no-ai]

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
echo "  NeuStack Linux Setup"
echo "=============================================="
echo "  Project: $PROJECT_ROOT"
echo "=============================================="
echo ""

# ─── Helper: install packages ───
install_packages() {
    local pkgs=("$@")
    echo "  Installing: ${pkgs[*]}..."
    if command -v apt-get &> /dev/null; then
        apt-get update -qq
        apt-get install -y -qq "${pkgs[@]}"
    elif command -v dnf &> /dev/null; then
        dnf install -y "${pkgs[@]}"
    elif command -v yum &> /dev/null; then
        yum install -y "${pkgs[@]}"
    elif command -v pacman &> /dev/null; then
        pacman -Sy --noconfirm "${pkgs[@]}"
    else
        echo "  ERROR: Unknown package manager. Please install manually: ${pkgs[*]}"
        exit 1
    fi
}

# ─── 1. Check dependencies ───
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

MISSING_PKGS=()

# Map command → package name (apt style, adjust per distro in install_packages)
command -v cmake  &>/dev/null || MISSING_PKGS+=(cmake)
command -v g++    &>/dev/null || MISSING_PKGS+=(g++)
command -v curl   &>/dev/null || MISSING_PKGS+=(curl)
command -v socat  &>/dev/null || MISSING_PKGS+=(socat)
command -v clang  &>/dev/null || MISSING_PKGS+=(clang)
command -v pkg-config &>/dev/null || MISSING_PKGS+=(pkg-config)

# Ninja: preferred but optional
HAS_NINJA=0
if command -v ninja &> /dev/null; then
    HAS_NINJA=1
else
    # Try to install ninja
    MISSING_PKGS+=(ninja-build)
fi

# Catch2: needed for tests
if ! dpkg -s catch2 &>/dev/null 2>&1; then
    MISSING_PKGS+=(catch2)
fi

# libbpf + libelf + zlib: needed for AF_XDP / BPF compilation
if ! pkg-config --exists libbpf 2>/dev/null; then
    if command -v apt-get &>/dev/null; then
        MISSING_PKGS+=(libbpf-dev)
    elif command -v dnf &>/dev/null || command -v yum &>/dev/null; then
        MISSING_PKGS+=(libbpf-devel)
    elif command -v pacman &>/dev/null; then
        MISSING_PKGS+=(libbpf)
    fi
fi
if command -v apt-get &>/dev/null; then
    dpkg -s libelf-dev  &>/dev/null 2>&1 || MISSING_PKGS+=(libelf-dev)
    dpkg -s zlib1g-dev  &>/dev/null 2>&1 || MISSING_PKGS+=(zlib1g-dev)
elif command -v dnf &>/dev/null || command -v yum &>/dev/null; then
    rpm -q elfutils-libelf-devel &>/dev/null 2>&1 || MISSING_PKGS+=(elfutils-libelf-devel)
    rpm -q zlib-devel &>/dev/null 2>&1 || MISSING_PKGS+=(zlib-devel)
elif command -v pacman &>/dev/null; then
    pacman -Q libelf &>/dev/null 2>&1 || MISSING_PKGS+=(libelf)
    pacman -Q zlib   &>/dev/null 2>&1 || MISSING_PKGS+=(zlib)
fi

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    install_packages "${MISSING_PKGS[@]}"
fi

# Re-check ninja after install attempt
if command -v ninja &> /dev/null; then
    HAS_NINJA=1
fi

# Show status
check_cmd cmake
check_cmd g++
check_cmd curl
check_cmd socat
if [ $HAS_NINJA -eq 1 ]; then
    echo "  ✓ ninja"
else
    echo "  - ninja (not found, will use make)"
fi

# AF_XDP readiness: need both clang and libbpf
HAS_AFXDP=0
if command -v clang &>/dev/null && pkg-config --exists libbpf 2>/dev/null; then
    HAS_AFXDP=1
    echo "  ✓ AF_XDP ready (clang + libbpf)"
else
    echo "  - AF_XDP not ready (need clang + libbpf)"
fi

# Check g++ version (need C++20 → g++ >= 10)
GCC_VER=$(g++ -dumpversion | cut -d. -f1)
if [ "$GCC_VER" -lt 10 ]; then
    echo ""
    echo "  ERROR: g++ $GCC_VER is too old, need >= 10 for C++20"
    echo ""
    echo "  Fix:"
    echo "    apt install g++-12 && export CXX=g++-12"
    echo "    # or"
    echo "    dnf install gcc-toolset-12-gcc-c++"
    exit 1
fi
echo "  ✓ g++ version $GCC_VER (C++20 OK)"

# ─── 2. TUN module ───
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

# Detect architecture
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64)  ORT_PLATFORM="linux-x64" ;;
    aarch64) ORT_PLATFORM="linux-aarch64" ;;
    *)
        echo "  WARNING: Unsupported architecture '$ARCH' for ONNX Runtime"
        echo "  AI features will be disabled"
        ORT_PLATFORM=""
        ;;
esac
echo "  Architecture: $ARCH → ${ORT_PLATFORM:-unsupported}"

ORT_FOUND=0
if [ -n "$ORT_PLATFORM" ]; then
    ORT_DIR="$PROJECT_ROOT/third_party/onnxruntime/$ORT_PLATFORM"

    if [ -f "$ORT_DIR/lib/libonnxruntime.so" ]; then
        echo "  ✓ ONNX Runtime already installed"
        ORT_FOUND=1
    else
        echo "  Downloading ONNX Runtime ($ORT_PLATFORM)..."
        if bash "$PROJECT_ROOT/scripts/download/download_onnxruntime.sh" "$ORT_PLATFORM"; then
            ORT_FOUND=1
            echo "  ✓ ONNX Runtime downloaded"
        else
            echo "  ✗ Download failed, AI features will be disabled"
        fi
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

# ─── 4. Build ───
echo ""
echo "[4/4] Building NeuStack..."

BUILD_DIR="$PROJECT_ROOT/build"
mkdir -p "$BUILD_DIR"

# 清理旧的 CMake 缓存 (避免 generator 切换冲突)
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    OLD_GEN=$(grep "^CMAKE_GENERATOR:" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
    if [ $HAS_NINJA -eq 1 ] && [ "$OLD_GEN" != "Ninja" ]; then
        echo "  Cleaning stale CMake cache (was $OLD_GEN, switching to Ninja)..."
        rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
    elif [ $HAS_NINJA -eq 0 ] && [ "$OLD_GEN" = "Ninja" ]; then
        echo "  Cleaning stale CMake cache (was Ninja, switching to Make)..."
        rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
    fi
fi

# Determine AF_XDP flag
if [ $HAS_AFXDP -eq 1 ]; then
    ENABLE_AFXDP=ON
else
    ENABLE_AFXDP=OFF
fi

# Select generator
CMAKE_ARGS=(
    -DNEUSTACK_ENABLE_AI=$ENABLE_AI
    -DNEUSTACK_ENABLE_AF_XDP=$ENABLE_AFXDP
    -DCMAKE_BUILD_TYPE=Release
)
if [ $HAS_NINJA -eq 1 ]; then
    CMAKE_ARGS+=(-G Ninja)
    BUILD_CMD="ninja"
else
    BUILD_CMD="make -j$(nproc)"
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
echo "  AF_XDP:      $ENABLE_AFXDP"
echo "  Binary:      $BUILD_DIR/examples/neustack_demo"
echo ""
echo "  Quick start:"
echo "    # Start the stack (requires root for TUN)"
echo "    sudo ./build/examples/neustack_demo --ip 10.0.0.2 -v"
echo ""
echo "    # With data collection"
echo "    sudo ./build/examples/neustack_demo --ip 10.0.0.2 --collect -v"
echo ""
