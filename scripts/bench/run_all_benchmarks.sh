#!/usr/bin/env bash
# run_all_benchmarks.sh — 自动运行所有 NeuStack benchmark 并保存结果
# Usage: bash scripts/bench/run_all_benchmarks.sh [build_dir]
#
# 默认 build_dir = build/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/build}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: build directory '$BUILD_DIR' not found."
    echo "Usage: $0 [build_dir]"
    exit 1
fi

# Resolve to absolute path
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

# Create results directory
TIMESTAMP="$(date '+%Y%m%d_%H%M%S')"
RESULTS_DIR="${PROJECT_ROOT}/bench_results/${TIMESTAMP}"
mkdir -p "$RESULTS_DIR"

echo "============================================"
echo "  NeuStack Benchmark Suite"
echo "  Results: ${RESULTS_DIR}"
echo "============================================"
echo ""

# Collect environment info
echo "--- Collecting environment info ---"
bash "$SCRIPT_DIR/collect_env.sh" | tee "$RESULTS_DIR/environment.txt"
echo ""

# Find all bench_* executables
BENCH_EXECS=()
for exe in "$BUILD_DIR/tests/bench_"*; do
    if [ -x "$exe" ] && [ -f "$exe" ]; then
        BENCH_EXECS+=("$exe")
    fi
done

if [ ${#BENCH_EXECS[@]} -eq 0 ]; then
    echo "Error: no bench_* executables found in $BUILD_DIR/tests/"
    echo "Did you compile with: cmake --build $BUILD_DIR"
    exit 1
fi

echo "Found ${#BENCH_EXECS[@]} benchmark(s):"
for exe in "${BENCH_EXECS[@]}"; do
    echo "  - $(basename "$exe")"
done
echo ""

# Run each benchmark
PASSED=0
FAILED=0
SUMMARY=""

for exe in "${BENCH_EXECS[@]}"; do
    name="$(basename "$exe")"
    result_file="$RESULTS_DIR/${name}.txt"

    echo "============================================"
    echo "  Running: $name"
    echo "============================================"

    if "$exe" 2>&1 | tee "$result_file"; then
        PASSED=$((PASSED + 1))
        SUMMARY="${SUMMARY}  [PASS] ${name}\n"
    else
        FAILED=$((FAILED + 1))
        SUMMARY="${SUMMARY}  [FAIL] ${name}\n"
    fi
    echo ""
done

# Print summary
echo "============================================"
echo "  Summary"
echo "============================================"
printf "%b" "$SUMMARY"
echo ""
echo "Total: $((PASSED + FAILED))  Passed: ${PASSED}  Failed: ${FAILED}"
echo "Results saved to: ${RESULTS_DIR}"
echo "============================================"

exit $FAILED
