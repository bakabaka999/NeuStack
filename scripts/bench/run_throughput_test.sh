#!/usr/bin/env bash
# run_throughput_test.sh — 端到端吞吐量消融实验
#
# 使用 veth pair + network namespace 在单机上测试
# TUN vs AF_XDP (generic) 的真实包收发吞吐量。
#
# 不需要真实网卡，不需要两台机器。
# 需要 root 权限（创建 namespace 和 veth）。
#
# Usage:
#   sudo bash scripts/bench/run_throughput_test.sh [--duration 10] [--runs 3] [--payload 64]
#
# 架构:
#   ┌── ns_neustack ──┐   veth pair   ┌──── ns_gen ─────┐
#   │                 │               │                  │
#   │  NeuStack 收包  │←── veth0/1 ──→│  UDP flood 发包  │
#   │  (TUN/AF_XDP)   │               │  (raw socket)    │
#   │                 │               │                  │
#   └─────────────────┘               └──────────────────┘

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────
DURATION=10
NUM_RUNS=3
PAYLOAD_SIZE=64
BUILD_DIR="${PROJECT_ROOT}/build"

# ─────────────────────────────────────────────────────────
# Parse arguments
# ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --duration)  DURATION="$2"; shift 2 ;;
        --runs)      NUM_RUNS="$2"; shift 2 ;;
        --payload)   PAYLOAD_SIZE="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: sudo $0 [options]"
            echo ""
            echo "Options:"
            echo "  --duration  N   Test duration per run in seconds (default: 10)"
            echo "  --runs     N    Number of runs per configuration (default: 3)"
            echo "  --payload  N    UDP payload size in bytes (default: 64)"
            echo "  --build-dir D   Build directory (default: build/)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ─────────────────────────────────────────────────────────
# Checks
# ─────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: this script requires root. Run with sudo."
    exit 1
fi

BENCH_EXE="${BUILD_DIR}/tests/bench_e2e_throughput"
if [ ! -x "$BENCH_EXE" ]; then
    echo "Error: $BENCH_EXE not found."
    echo "Build first: cmake --build ${BUILD_DIR} --target bench_e2e_throughput"
    exit 1
fi

# Check for nping or our own flood mode
FLOOD_TOOL="bench"  # use our own flood mode

echo "============================================"
echo "  NeuStack E2E Throughput Test"
echo "============================================"
echo "  Duration:  ${DURATION}s per run"
echo "  Runs:      ${NUM_RUNS}"
echo "  Payload:   ${PAYLOAD_SIZE} bytes"
echo "  Build:     ${BUILD_DIR}"
echo "============================================"
echo ""

# ─────────────────────────────────────────────────────────
# Output directory
# ─────────────────────────────────────────────────────────
TIMESTAMP="$(date '+%Y%m%d_%H%M%S')"
RESULTS_DIR="${PROJECT_ROOT}/bench_results/${TIMESTAMP}/e2e"
mkdir -p "$RESULTS_DIR"

# ─────────────────────────────────────────────────────────
# Network namespace setup
# ─────────────────────────────────────────────────────────

NS_SINK="ns_neustack"
NS_GEN="ns_gen"
VETH_SINK="veth_sink"
VETH_GEN="veth_gen"
IP_SINK="10.0.0.1"
IP_GEN="10.0.0.2"

cleanup() {
    echo ""
    echo "--- Cleaning up ---"
    # Kill any remaining background processes
    kill $(jobs -p) 2>/dev/null || true
    wait 2>/dev/null || true

    ip netns del "$NS_SINK" 2>/dev/null || true
    ip netns del "$NS_GEN" 2>/dev/null || true
    echo "  Namespaces removed."
}

trap cleanup EXIT

setup_veth() {
    echo "--- Setting up veth + namespaces ---"

    # Clean up any leftovers
    ip netns del "$NS_SINK" 2>/dev/null || true
    ip netns del "$NS_GEN" 2>/dev/null || true

    # Create namespaces
    ip netns add "$NS_SINK"
    ip netns add "$NS_GEN"

    # Create veth pair
    ip link add "$VETH_SINK" type veth peer name "$VETH_GEN"

    # Move to namespaces
    ip link set "$VETH_SINK" netns "$NS_SINK"
    ip link set "$VETH_GEN" netns "$NS_GEN"

    # Configure sink side
    ip netns exec "$NS_SINK" ip addr add "${IP_SINK}/24" dev "$VETH_SINK"
    ip netns exec "$NS_SINK" ip link set "$VETH_SINK" up
    ip netns exec "$NS_SINK" ip link set lo up

    # Configure generator side
    ip netns exec "$NS_GEN" ip addr add "${IP_GEN}/24" dev "$VETH_GEN"
    ip netns exec "$NS_GEN" ip link set "$VETH_GEN" up
    ip netns exec "$NS_GEN" ip link set lo up

    # Verify connectivity
    if ip netns exec "$NS_GEN" ping -c 1 -W 1 "$IP_SINK" &>/dev/null; then
        echo "  veth pair OK: ${IP_GEN} <-> ${IP_SINK}"
    else
        echo "  WARNING: ping failed, but may still work for raw sockets"
    fi
    echo ""
}

# ─────────────────────────────────────────────────────────
# Run a single throughput test
# ─────────────────────────────────────────────────────────

run_single_test() {
    local device_type="$1"
    local run_num="$2"
    local output_file="$3"

    local ifname_arg=""
    if [ "$device_type" = "af_xdp" ]; then
        ifname_arg="--ifname $VETH_SINK"
    fi

    # Start the sink in the sink namespace
    ip netns exec "$NS_SINK" "$BENCH_EXE" \
        --mode sink \
        --device "$device_type" \
        $ifname_arg \
        --duration "$DURATION" \
        --json \
        > "$output_file" 2>/dev/null &
    local sink_pid=$!

    # Give sink time to start
    sleep 1

    # Start the flood in the generator namespace using raw UDP
    # Use nping if available, otherwise use Python
    ip netns exec "$NS_GEN" bash -c "
        python3 -c \"
import socket, time, struct
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
payload = b'\\xaa' * ${PAYLOAD_SIZE}
target = ('${IP_SINK}', 9999)
deadline = time.monotonic() + ${DURATION}
count = 0
while time.monotonic() < deadline:
    try:
        sock.sendto(payload, target)
        count += 1
    except:
        pass
sock.close()
\" 2>/dev/null
    " &
    local gen_pid=$!

    # Wait for sink to finish
    wait $sink_pid 2>/dev/null || true
    kill $gen_pid 2>/dev/null || true
    wait $gen_pid 2>/dev/null || true
}

# ─────────────────────────────────────────────────────────
# Run throughput tests for each configuration
# ─────────────────────────────────────────────────────────

declare -a CONFIGS=("tun")

# Check if AF_XDP build is available
if "$BENCH_EXE" --help 2>&1 | grep -q "af_xdp" || \
   strings "$BENCH_EXE" 2>/dev/null | grep -q "af_xdp"; then
    CONFIGS+=("af_xdp")
fi

for config in "${CONFIGS[@]}"; do
    echo "============================================"
    echo "  Configuration: $config"
    echo "============================================"

    setup_veth

    raw_file="${RESULTS_DIR}/${config}_raw.json"
    echo "[" > "$raw_file"

    success=0
    for run in $(seq 1 "$NUM_RUNS"); do
        echo "  Run $run/$NUM_RUNS..."

        out_file=$(mktemp)
        run_single_test "$config" "$run" "$out_file"

        if [ -s "$out_file" ] && python3 -m json.tool "$out_file" &>/dev/null; then
            if [ "$success" -gt 0 ]; then
                echo "," >> "$raw_file"
            fi
            cat "$out_file" >> "$raw_file"
            success=$((success + 1))

            # Print quick stats
            mpps=$(python3 -c "import json; d=json.load(open('$out_file')); print(f\"{d['results']['mpps']:.4f}\")" 2>/dev/null || echo "?")
            echo "    -> ${mpps} Mpps"
        else
            echo "    -> FAILED"
        fi
        rm -f "$out_file"

        # Recreate veth for clean state between runs
        cleanup 2>/dev/null || true
    done

    echo "]" >> "$raw_file"
    echo "  Completed: $success/$NUM_RUNS runs"
    echo ""
done

# ─────────────────────────────────────────────────────────
# Post-processing
# ─────────────────────────────────────────────────────────

echo "--- Post-processing ---"

python3 -c "
import json, os

results_dir = '$RESULTS_DIR'
configs = $(printf "'%s', " "${CONFIGS[@]}" | sed 's/, $//')
config_list = [$configs]

summary = {
    'benchmark': 'e2e_throughput',
    'timestamp': '$(date -u +%Y-%m-%dT%H:%M:%SZ)',
    'configs': {}
}

for config in config_list:
    raw_path = os.path.join(results_dir, f'{config}_raw.json')
    if not os.path.exists(raw_path):
        continue
    with open(raw_path) as f:
        try:
            runs = json.load(f)
        except json.JSONDecodeError:
            continue
    if not runs:
        continue

    mpps_values = [r['results']['mpps'] for r in runs if 'results' in r]
    gbps_values = [r['results']['gbps'] for r in runs if 'results' in r]
    pps_values = [r['results']['pps'] for r in runs if 'results' in r]

    if mpps_values:
        import statistics
        summary['configs'][config] = {
            'num_runs': len(mpps_values),
            'mpps': {
                'mean': statistics.mean(mpps_values),
                'std': statistics.stdev(mpps_values) if len(mpps_values) > 1 else 0,
                'min': min(mpps_values),
                'max': max(mpps_values),
            },
            'gbps': {
                'mean': statistics.mean(gbps_values),
                'std': statistics.stdev(gbps_values) if len(gbps_values) > 1 else 0,
            },
            'pps': {
                'mean': statistics.mean(pps_values),
            },
            'runs': runs,
        }

out_path = os.path.join(results_dir, 'summary.json')
with open(out_path, 'w') as f:
    json.dump(summary, f, indent=2)
print(f'Summary: {out_path}')
" || echo "  Post-processing failed"

ln -sfn "$RESULTS_DIR" "${PROJECT_ROOT}/bench_results/latest_e2e"

echo ""
echo "============================================"
echo "  Results: $RESULTS_DIR"
echo "============================================"
echo ""
echo "To compare:"
echo "  cat $RESULTS_DIR/summary.json | python3 -m json.tool"
