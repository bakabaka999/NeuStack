#!/bin/bash
# scripts/linux/setup.sh
#
# Linux environment setup & build
#
# What it does:
#   1. Check / install dependencies (cmake, g++, clang++, ninja, socat)
#   2. Check TUN module
#   3. Download ONNX Runtime if needed (auto-detect arch)
#   4. Build NeuStack (auto-enable AI if ORT found)
#
# Usage:
#   sudo bash scripts/linux/setup.sh [--no-ai] [--with-fuzzers] [--with-tls]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Parse args
FORCE_NO_AI=0
WITH_FUZZERS=0
WITH_TLS=0
for arg in "$@"; do
    case "$arg" in
        --no-ai) FORCE_NO_AI=1 ;;
        --with-fuzzers) WITH_FUZZERS=1 ;;
        --with-tls) WITH_TLS=1 ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--no-ai] [--with-fuzzers] [--with-tls]"; exit 1 ;;
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

# ── Step A: 基础工具（必须先装，后续检测依赖它们）──
BASIC_PKGS=()
command -v cmake      &>/dev/null || BASIC_PKGS+=(cmake)
command -v g++        &>/dev/null || BASIC_PKGS+=(g++)
command -v clang++    &>/dev/null || BASIC_PKGS+=(clang)
command -v curl       &>/dev/null || BASIC_PKGS+=(curl)
command -v socat      &>/dev/null || BASIC_PKGS+=(socat)
command -v clang      &>/dev/null || BASIC_PKGS+=(clang)
command -v pkg-config &>/dev/null || BASIC_PKGS+=(pkg-config)
command -v ninja      &>/dev/null || BASIC_PKGS+=(ninja-build)

if [ ${#BASIC_PKGS[@]} -gt 0 ]; then
    install_packages "${BASIC_PKGS[@]}"
fi

# ── Step B: 库依赖（pkg-config 现在肯定已就绪）──
LIB_PKGS=()

# Catch2: 包名因发行版/版本而异，且系统版本可能是 v2（太旧）
# CMake 找不到 Catch2 v3 时会自动通过 FetchContent 下载，所以这里只是尽力安装
if command -v apt-get &>/dev/null; then
    if ! dpkg -s libcatch2-dev &>/dev/null 2>&1 && ! dpkg -s catch2 &>/dev/null 2>&1; then
        # Ubuntu 22.04+ 用 libcatch2-dev，Debian 12+ 用 catch2，先探测再装
        if apt-cache show libcatch2-dev &>/dev/null 2>&1; then
            LIB_PKGS+=(libcatch2-dev)
        elif apt-cache show catch2 &>/dev/null 2>&1; then
            LIB_PKGS+=(catch2)
        else
            echo "  - Catch2 not in apt repos, CMake will fetch from source"
        fi
    fi
elif command -v dnf &>/dev/null || command -v yum &>/dev/null; then
    rpm -q catch2-devel &>/dev/null 2>&1 || LIB_PKGS+=(catch2-devel)
elif command -v pacman &>/dev/null; then
    pacman -Q catch2 &>/dev/null 2>&1 || LIB_PKGS+=(catch2)
fi

# libbpf + libelf + zlib + linux-libc-dev: AF_XDP / BPF 编译需要
if ! pkg-config --exists libbpf 2>/dev/null; then
    if command -v apt-get &>/dev/null; then
        LIB_PKGS+=(libbpf-dev)
    elif command -v dnf &>/dev/null || command -v yum &>/dev/null; then
        LIB_PKGS+=(libbpf-devel)
    elif command -v pacman &>/dev/null; then
        LIB_PKGS+=(libbpf)
    fi
fi
if command -v apt-get &>/dev/null; then
    dpkg -s libelf-dev    &>/dev/null 2>&1 || LIB_PKGS+=(libelf-dev)
    dpkg -s zlib1g-dev    &>/dev/null 2>&1 || LIB_PKGS+=(zlib1g-dev)
    dpkg -s linux-libc-dev &>/dev/null 2>&1 || LIB_PKGS+=(linux-libc-dev)
elif command -v dnf &>/dev/null || command -v yum &>/dev/null; then
    rpm -q elfutils-libelf-devel &>/dev/null 2>&1 || LIB_PKGS+=(elfutils-libelf-devel)
    rpm -q zlib-devel            &>/dev/null 2>&1 || LIB_PKGS+=(zlib-devel)
    rpm -q kernel-headers        &>/dev/null 2>&1 || LIB_PKGS+=(kernel-headers)
elif command -v pacman &>/dev/null; then
    pacman -Q libelf       &>/dev/null 2>&1 || LIB_PKGS+=(libelf)
    pacman -Q zlib         &>/dev/null 2>&1 || LIB_PKGS+=(zlib)
    pacman -Q linux-headers &>/dev/null 2>&1 || LIB_PKGS+=(linux-headers)
fi

if [ ${#LIB_PKGS[@]} -gt 0 ]; then
    install_packages "${LIB_PKGS[@]}"
fi

# BPF 编译需要 <asm/types.h>，clang -target bpf 不搜索架构特定目录
# Ubuntu/Debian 上 asm 头文件在 /usr/include/<arch>-linux-gnu/asm，需要软链接
if [ ! -e /usr/include/asm ]; then
    ARCH_DIR="/usr/include/$(uname -m)-linux-gnu/asm"
    if [ -d "$ARCH_DIR" ]; then
        ln -s "$ARCH_DIR" /usr/include/asm
        echo "  ✓ Created /usr/include/asm → $ARCH_DIR symlink"
    fi
fi

# Re-check ninja after install
HAS_NINJA=0
if command -v ninja &> /dev/null; then
    HAS_NINJA=1
fi

# Show status
check_cmd cmake
check_cmd g++
check_cmd clang++
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

ENABLE_FUZZERS=OFF
SELECTED_CC="$(command -v cc || command -v gcc)"
SELECTED_CXX="$(command -v c++ || command -v g++)"

if [ $WITH_FUZZERS -eq 1 ]; then
    if ! command -v clang++ &>/dev/null || ! command -v clang &>/dev/null; then
        echo ""
        echo "  ERROR: --with-fuzzers requires clang/clang++"
        exit 1
    fi

    SELECTED_CC="$(command -v clang)"
    SELECTED_CXX="$(command -v clang++)"
    ENABLE_FUZZERS=ON
    echo "  ✓ Fuzz compiler: $SELECTED_CXX"
fi

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

# TLS setup
ENABLE_TLS=OFF
if [ $WITH_TLS -eq 1 ]; then
    MBEDTLS_DIR="$PROJECT_ROOT/third_party/mbedtls"
    if [ -f "$MBEDTLS_DIR/CMakeLists.txt" ]; then
        echo "  ✓ mbedTLS found"
        ENABLE_TLS=ON
    else
        echo "  Downloading mbedTLS v3.6.0..."
        mkdir -p "$PROJECT_ROOT/third_party"
        if curl -L https://github.com/Mbed-TLS/mbedtls/releases/download/v3.6.0/mbedtls-3.6.0.tar.bz2 -o /tmp/mbedtls-3.6.0.tar.bz2 && \
           tar xjf /tmp/mbedtls-3.6.0.tar.bz2 -C "$PROJECT_ROOT/third_party" && \
           mv "$PROJECT_ROOT/third_party/mbedtls-3.6.0" "$MBEDTLS_DIR"; then
            echo "  ✓ mbedTLS downloaded"
            ENABLE_TLS=ON
        else
            echo "  ✗ mbedTLS download failed, TLS will be disabled"
        fi
    fi
    # Install python3 if needed (mbedTLS build scripts may need it)
    command -v python3 &>/dev/null || install_packages python3
fi

BUILD_DIR="$PROJECT_ROOT/build"
mkdir -p "$BUILD_DIR"

# 清理旧的 CMake 缓存 (避免 generator 切换冲突)
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    OLD_GEN=$(grep "^CMAKE_GENERATOR:" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)
    OLD_CXX=$(grep "^CMAKE_CXX_COMPILER:FILEPATH=" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2-)
    if [ $HAS_NINJA -eq 1 ] && [ "$OLD_GEN" != "Ninja" ]; then
        echo "  Cleaning stale CMake cache (was $OLD_GEN, switching to Ninja)..."
        rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
    elif [ $HAS_NINJA -eq 0 ] && [ "$OLD_GEN" = "Ninja" ]; then
        echo "  Cleaning stale CMake cache (was Ninja, switching to Make)..."
        rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
    elif [ -n "$OLD_CXX" ] && [ "$OLD_CXX" != "$SELECTED_CXX" ]; then
        echo "  Cleaning stale CMake cache (compiler changed to $SELECTED_CXX)..."
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
    -DNEUSTACK_ENABLE_TLS=$ENABLE_TLS
    -DNEUSTACK_BUILD_FUZZERS=$ENABLE_FUZZERS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=$SELECTED_CC
    -DCMAKE_CXX_COMPILER=$SELECTED_CXX
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
echo "  TLS:         $ENABLE_TLS"
echo "  Fuzzers:     $ENABLE_FUZZERS"
echo "  Binary:      $BUILD_DIR/examples/neustack_demo"
echo ""
echo "  Quick start:"
echo "    # Start the stack (requires root for TUN)"
echo "    sudo ./build/examples/neustack_demo --ip 10.0.0.2 -v"
echo ""
echo "    # With data collection"
echo "    sudo ./build/examples/neustack_demo --ip 10.0.0.2 --collect -v"
echo ""
if [ "$ENABLE_FUZZERS" = "ON" ]; then
    echo "    # Run parser fuzzers"
    echo "    ./build/fuzz/fuzz_http_parser -runs=100"
    echo "    ./build/fuzz/fuzz_dns_parser -runs=100"
    echo ""
fi
