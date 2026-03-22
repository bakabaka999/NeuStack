#!/usr/bin/env bash
# run_ablation.sh — Layer 3 消融实验：TUN vs AF_XDP × AI ON/OFF
#
# 此脚本在真机上运行，不适用于容器环境。
# 它会构建多个配置并依次运行 benchmark，收集 JSON 结果。
#
# Usage:
#   bash scripts/bench/run_ablation.sh [--runs N] [--interface IFACE]
#
# 产出目录: bench_results/<timestamp>/ablation/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────
NUM_RUNS=5
INTERFACE=""
BUILD_TYPE="Release"

# ─────────────────────────────────────────────────────────
# Parse arguments
# ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --runs)
            NUM_RUNS="$2"; shift 2 ;;
        --interface)
            INTERFACE="$2"; shift 2 ;;
        --build-type)
            BUILD_TYPE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--runs N] [--interface IFACE] [--build-type TYPE]"
            echo ""
            echo "Options:"
            echo "  --runs N          Number of runs per configuration (default: 5)"
            echo "  --interface IFACE Network interface for AF_XDP tests"
            echo "  --build-type TYPE CMake build type (default: Release)"
            exit 0 ;;
        *)
            echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ─────────────────────────────────────────────────────────
# Environment checks
# ─────────────────────────────────────────────────────────
echo "============================================"
echo "  NeuStack Ablation Experiment"
echo "============================================"
echo ""

# Check for root (needed for AF_XDP)
if [ "$(id -u)" -ne 0 ]; then
    echo "WARNING: AF_XDP tests typically require root privileges."
    echo "         Consider running with sudo."
    echo ""
fi

# Check for real network interface
if [ -n "$INTERFACE" ]; then
    if ! ip link show "$INTERFACE" &>/dev/null; then
        echo "Error: interface '$INTERFACE' not found."
        exit 1
    fi
    echo "Using interface: $INTERFACE"
else
    # Auto-detect default interface
    INTERFACE=$(ip route 2>/dev/null | grep default | head -1 | awk '{print $5}' || true)
    if [ -z "$INTERFACE" ]; then
        echo "WARNING: no default network interface detected."
        echo "         Pass --interface IFACE to specify one."
    else
        echo "Auto-detected interface: $INTERFACE"
    fi
fi

# Check if running in container
if [ -f /.dockerenv ] || grep -q docker /proc/1/cgroup 2>/dev/null; then
    echo ""
    echo "WARNING: Running inside a container."
    echo "         AF_XDP tests require a real machine with a supported NIC."
    echo "         Results may not be representative."
    echo ""
fi

echo ""

# ─────────────────────────────────────────────────────────
# Setup output directory
# ─────────────────────────────────────────────────────────
TIMESTAMP="$(date '+%Y%m%d_%H%M%S')"
RESULTS_DIR="${PROJECT_ROOT}/bench_results/${TIMESTAMP}/ablation"
mkdir -p "$RESULTS_DIR"

echo "Results directory: $RESULTS_DIR"
echo "Runs per config:  $NUM_RUNS"
echo ""

# Collect environment info
bash "$SCRIPT_DIR/collect_env.sh" > "$RESULTS_DIR/environment.txt" 2>&1 || true

# ─────────────────────────────────────────────────────────
# Build configurations
# ─────────────────────────────────────────────────────────

declare -A CONFIGS
CONFIGS=(
    ["tun_baseline"]="-DNEUSTACK_ENABLE_AF_XDP=OFF -DNEUSTACK_ENABLE_AI=OFF"
    ["afxdp_copy"]="-DNEUSTACK_ENABLE_AF_XDP=ON -DNEUSTACK_AF_XDP_ZEROCOPY=OFF -DNEUSTACK_ENABLE_AI=OFF"
    ["afxdp_zerocopy"]="-DNEUSTACK_ENABLE_AF_XDP=ON -DNEUSTACK_AF_XDP_ZEROCOPY=ON -DNEUSTACK_ENABLE_AI=OFF"
    ["afxdp_zerocopy_ai"]="-DNEUSTACK_ENABLE_AF_XDP=ON -DNEUSTACK_AF_XDP_ZEROCOPY=ON -DNEUSTACK_ENABLE_AI=ON"
)

CONFIG_ORDER=("tun_baseline" "afxdp_copy" "afxdp_zerocopy" "afxdp_zerocopy_ai")

run_benchmark_n_times() {
    local exe="$1"
    local config_name="$2"
    local n="$3"
    local out_dir="$4"

    local raw_file="${out_dir}/${config_name}_raw.json"
    echo "[" > "$raw_file"

    local success=0
    for i in $(seq 1 "$n"); do
        echo "  Run $i/$n..."
        local tmp
        tmp=$(mktemp)
        if "$exe" --json > "$tmp" 2>/dev/null; then
            if [ "$success" -gt 0 ]; then
                echo "," >> "$raw_file"
            fi
            cat "$tmp" >> "$raw_file"
            success=$((success + 1))
        else
            echo "  WARNING: run $i failed"
        fi
        rm -f "$tmp"
    done

    echo "]" >> "$raw_file"
    echo "  Completed: $success/$n successful runs"
    return 0
}

# ─────────────────────────────────────────────────────────
# Build and run each configuration
# ─────────────────────────────────────────────────────────

for config_name in "${CONFIG_ORDER[@]}"; do
    cmake_flags="${CONFIGS[$config_name]}"

    echo "============================================"
    echo "  Configuration: $config_name"
    echo "  CMake flags:   $cmake_flags"
    echo "============================================"

    BUILD_DIR="${PROJECT_ROOT}/build_ablation_${config_name}"
    mkdir -p "$BUILD_DIR"

    # Configure
    echo "--- Configuring ---"
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        $cmake_flags \
        2>&1 | tail -5

    # Build
    echo "--- Building ---"
    cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tail -3

    # Find benchmark executable
    BENCH_EXE="${BUILD_DIR}/tests/bench_afxdp_datapath"
    if [ ! -x "$BENCH_EXE" ]; then
        echo "WARNING: bench_afxdp_datapath not found in $BUILD_DIR, skipping."
        echo ""
        continue
    fi

    # Run N times
    echo "--- Running ($NUM_RUNS runs) ---"
    run_benchmark_n_times "$BENCH_EXE" "$config_name" "$NUM_RUNS" "$RESULTS_DIR"
    echo ""
done

# ─────────────────────────────────────────────────────────
# Post-processing: generate combined summary
# ─────────────────────────────────────────────────────────

echo "============================================"
echo "  Post-Processing"
echo "============================================"

# Use benchmark_runner.py's statistics if Python is available
if command -v python3 &>/dev/null; then
    python3 -c "
import json, os, sys

results_dir = '$RESULTS_DIR'
config_list = ['tun_baseline', 'afxdp_copy', 'afxdp_zerocopy', 'afxdp_zerocopy_ai']

summary = {'ablation': {}, 'timestamp': '$(date -u +%Y-%m-%dT%H:%M:%SZ)'}

for config in config_list:
    raw_path = os.path.join(results_dir, f'{config}_raw.json')
    if not os.path.exists(raw_path):
        continue
    with open(raw_path) as f:
        runs = json.load(f)
    if not runs:
        continue
    summary['ablation'][config] = {
        'num_runs': len(runs),
        'runs': runs,
    }

out_path = os.path.join(results_dir, 'summary.json')
with open(out_path, 'w') as f:
    json.dump(summary, f, indent=2)
print(f'Summary written to {out_path}')
" || echo "  ERROR: Python post-processing failed"
else
    echo "  (Python not available, skipping post-processing)"
fi

# Create latest symlink
PARENT_DIR="$(dirname "$RESULTS_DIR")"
ln -sfn "$RESULTS_DIR" "${PROJECT_ROOT}/bench_results/latest"

# ─────────────────────────────────────────────────────────
# Cleanup build directories (optional)
# ─────────────────────────────────────────────────────────

echo ""
echo "============================================"
echo "  Results saved to: $RESULTS_DIR"
echo "============================================"
echo ""
echo "To generate figures:"
echo "  python3 scripts/bench/plot_results.py --input $RESULTS_DIR/summary.json"
echo ""
echo "Build directories (can be removed):"
for config_name in "${CONFIG_ORDER[@]}"; do
    echo "  build_ablation_${config_name}/"
done
