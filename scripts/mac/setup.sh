#!/bin/bash
# scripts/mac/setup.sh
#
# macOS environment setup & build
#
# What it does:
#   1. Check dependencies (cmake, clang++, ninja)
#   2. Optionally provision a libFuzzer-capable toolchain
#   3. Download ONNX Runtime if needed
#   4. Build NeuStack (auto-enable AI if ORT found)
#
# Usage:
#   bash scripts/mac/setup.sh [--no-ai] [--with-fuzzers]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Parse args
FORCE_NO_AI=0
WITH_FUZZERS=0
for arg in "$@"; do
    case "$arg" in
        --no-ai) FORCE_NO_AI=1 ;;
        --with-fuzzers) WITH_FUZZERS=1 ;;
        *) echo "Unknown option: $arg"; echo "Usage: $0 [--no-ai] [--with-fuzzers]"; exit 1 ;;
    esac
done

echo "=============================================="
echo "  NeuStack macOS Setup"
echo "=============================================="
echo "  Project: $PROJECT_ROOT"
echo "=============================================="
echo ""

# ─── 1. Check dependencies ───
echo "[1/4] Checking dependencies..."

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

SELECTED_CC="$(command -v clang)"
SELECTED_CXX="$(command -v clang++)"
ENABLE_FUZZERS=OFF

find_libfuzzer_runtime() {
    local clang_path="$1"
    local resource_dir
    local runtime_path

    if [ -z "$clang_path" ] || [ ! -x "$clang_path" ]; then
        return 1
    fi

    resource_dir="$("$clang_path" -print-resource-dir 2>/dev/null || true)"
    runtime_path="$resource_dir/lib/darwin/libclang_rt.fuzzer_osx.a"
    if [ -n "$resource_dir" ] && [ -f "$runtime_path" ]; then
        echo "$runtime_path"
        return 0
    fi

    return 1
}

if [ $WITH_FUZZERS -eq 1 ]; then
    echo ""
    echo "[2/4] Checking libFuzzer toolchain..."

    FUZZ_RUNTIME="$(find_libfuzzer_runtime "$SELECTED_CXX" || true)"
    if [ -n "$FUZZ_RUNTIME" ]; then
        echo "  ✓ libFuzzer runtime found via system clang++"
        echo "    $FUZZ_RUNTIME"
        ENABLE_FUZZERS=ON
    else
        XCODE_CLANG="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++"
        XCODE_CC="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"
        FUZZ_RUNTIME="$(find_libfuzzer_runtime "$XCODE_CLANG" || true)"
        if [ -n "$FUZZ_RUNTIME" ]; then
            SELECTED_CC="$XCODE_CC"
            SELECTED_CXX="$XCODE_CLANG"
            echo "  ✓ libFuzzer runtime found via Xcode toolchain"
            echo "    $FUZZ_RUNTIME"
            ENABLE_FUZZERS=ON
        else
            if command -v brew &>/dev/null; then
                BREW_LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
                if [ -z "$BREW_LLVM_PREFIX" ] || [ ! -x "$BREW_LLVM_PREFIX/bin/clang++" ]; then
                    echo "  Installing Homebrew LLVM (provides libFuzzer runtime)..."
                    brew install llvm
                    BREW_LLVM_PREFIX="$(brew --prefix llvm)"
                fi

                BREW_CC="$BREW_LLVM_PREFIX/bin/clang"
                BREW_CXX="$BREW_LLVM_PREFIX/bin/clang++"
                FUZZ_RUNTIME="$(find_libfuzzer_runtime "$BREW_CXX" || true)"
                if [ -n "$FUZZ_RUNTIME" ]; then
                    SELECTED_CC="$BREW_CC"
                    SELECTED_CXX="$BREW_CXX"
                    echo "  ✓ libFuzzer runtime found via Homebrew LLVM"
                    echo "    $FUZZ_RUNTIME"
                    ENABLE_FUZZERS=ON
                fi
            fi
        fi
    fi

    if [ "$ENABLE_FUZZERS" != "ON" ]; then
        echo "  ✗ libFuzzer runtime not found"
        echo ""
        echo "  Fix one of the following:"
        echo "    1. Install full Xcode"
        echo "    2. Or install Homebrew LLVM: brew install llvm"
        echo "    3. Then rerun: ./setup --with-fuzzers"
        exit 1
    fi

    echo "  Fuzz compiler: $SELECTED_CXX"
else
    echo ""
    echo "[2/4] libFuzzer toolchain check skipped"
fi

# ─── 3. ONNX Runtime ───
echo ""
echo "[3/4] Checking ONNX Runtime..."

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
    if bash "$PROJECT_ROOT/scripts/download/download_onnxruntime.sh" "$ORT_PLATFORM"; then
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

# ─── 4. Build ───
echo ""
echo "[4/4] Building NeuStack..."

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

# Select generator
CMAKE_ARGS=(
    -DNEUSTACK_ENABLE_AI=$ENABLE_AI
    -DNEUSTACK_BUILD_FUZZERS=$ENABLE_FUZZERS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=$SELECTED_CC
    -DCMAKE_CXX_COMPILER=$SELECTED_CXX
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
echo "  Fuzzers:     $ENABLE_FUZZERS"
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
if [ "$ENABLE_FUZZERS" = "ON" ]; then
    echo "    # Run parser fuzzers"
    echo "    ./build/fuzz/fuzz_http_parser -runs=100"
    echo "    ./build/fuzz/fuzz_dns_parser -runs=100"
    echo ""
fi
