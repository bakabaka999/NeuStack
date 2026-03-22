#!/usr/bin/env bash
# run_throughput_test.sh — 端到端吞吐量消融实验
#
# 使用 veth pair + network namespace 在单机上对比：
#   raw_socket (AF_PACKET recvfrom，模拟 TUN 路径)  vs  AF_XDP (ring buffer)
# 两种后端都绑定到同一个 veth 接口，公平对比。
#
# 需要 root 权限。不需要真实网卡或第二台机器。
#
# Usage:
#   sudo bash scripts/bench/run_throughput_test.sh [--duration 10] [--runs 3]
#
# 架构:
#   ┌──── ns_sink ────┐   veth pair   ┌──── ns_gen ─────┐
#   │                 │               │                  │
#   │  raw_socket 或  │←── veth0/1 ──→│  UDP flood 发包  │
#   │  AF_XDP 收包    │               │  (Python)        │
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
            echo "  --duration  N   Seconds per run (default: 10)"
            echo "  --runs     N    Runs per configuration (default: 3)"
            echo "  --payload  N    UDP payload bytes (default: 64)"
            echo "  --build-dir D   Build directory (default: build/)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ─────────────────────────────────────────────────────────
# Checks
# ─────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: requires root. Run with sudo."
    exit 1
fi

BENCH_EXE="${BUILD_DIR}/tests/bench_e2e_throughput"
if [ ! -x "$BENCH_EXE" ]; then
    echo "Error: $BENCH_EXE not found."
    echo "Build: cmake --build ${BUILD_DIR} --target bench_e2e_throughput"
    exit 1
fi

echo "============================================"
echo "  NeuStack E2E Throughput Test"
echo "============================================"
echo "  Duration:  ${DURATION}s per run"
echo "  Runs:      ${NUM_RUNS}"
echo "  Payload:   ${PAYLOAD_SIZE} bytes"
echo "============================================"
echo ""

# ─────────────────────────────────────────────────────────
# Output directory
# ─────────────────────────────────────────────────────────
TIMESTAMP="$(date '+%Y%m%d_%H%M%S')"
RESULTS_DIR="${PROJECT_ROOT}/bench_results/${TIMESTAMP}/e2e"
mkdir -p "$RESULTS_DIR"

# ─────────────────────────────────────────────────────────
# veth + namespace helpers
# ─────────────────────────────────────────────────────────
NS_SINK="ns_sink"
NS_GEN="ns_gen"
VETH_SINK="veth_sink"
VETH_GEN="veth_gen"
IP_SINK="10.0.0.1"
IP_GEN="10.0.0.2"

teardown_ns() {
    # Kill lingering processes
    ip netns pids "$NS_SINK" 2>/dev/null | xargs -r kill 2>/dev/null || true
    ip netns pids "$NS_GEN" 2>/dev/null | xargs -r kill 2>/dev/null || true
    sleep 0.5
    ip netns del "$NS_SINK" 2>/dev/null || true
    ip netns del "$NS_GEN" 2>/dev/null || true
}

setup_ns() {
    teardown_ns

    ip netns add "$NS_SINK"
    ip netns add "$NS_GEN"

    ip link add "$VETH_SINK" type veth peer name "$VETH_GEN"
    ip link set "$VETH_SINK" netns "$NS_SINK"
    ip link set "$VETH_GEN" netns "$NS_GEN"

    ip netns exec "$NS_SINK" ip addr add "${IP_SINK}/24" dev "$VETH_SINK"
    ip netns exec "$NS_SINK" ip link set "$VETH_SINK" up
    ip netns exec "$NS_SINK" ip link set lo up

    ip netns exec "$NS_GEN" ip addr add "${IP_GEN}/24" dev "$VETH_GEN"
    ip netns exec "$NS_GEN" ip link set "$VETH_GEN" up
    ip netns exec "$NS_GEN" ip link set lo up

    # Wait for link up
    sleep 0.3

    # Verify
    if ip netns exec "$NS_GEN" ping -c 1 -W 1 "$IP_SINK" &>/dev/null; then
        echo "  veth OK: ${IP_GEN} <-> ${IP_SINK}"
    else
        echo "  WARNING: ping failed (may still work)"
    fi
}

trap teardown_ns EXIT

# ─────────────────────────────────────────────────────────
# UDP flood generator (runs in ns_gen)
# ─────────────────────────────────────────────────────────
start_flood() {
    local duration="$1"
    ip netns exec "$NS_GEN" python3 -c "
import socket, time
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
payload = b'\\xaa' * ${PAYLOAD_SIZE}
target = ('${IP_SINK}', 9999)
end = time.monotonic() + ${duration}
while time.monotonic() < end:
    sock.sendto(payload, target)
sock.close()
" &
    FLOOD_PID=$!
}

stop_flood() {
    kill "$FLOOD_PID" 2>/dev/null || true
    wait "$FLOOD_PID" 2>/dev/null || true
}

# ─────────────────────────────────────────────────────────
# Configurations to test
# ─────────────────────────────────────────────────────────
# Always try both - the benchmark will fail gracefully if AF_XDP isn't compiled
CONFIGS="raw_socket af_xdp"

echo "  Configs: $CONFIGS"
echo ""

# ─────────────────────────────────────────────────────────
# Run
# ─────────────────────────────────────────────────────────
for config in $CONFIGS; do
    echo "============================================"
    echo "  Configuration: $config"
    echo "============================================"

    raw_file="${RESULTS_DIR}/${config}_raw.json"
    echo "[" > "$raw_file"
    success=0

    for run in $(seq 1 "$NUM_RUNS"); do
        echo "  Run $run/$NUM_RUNS..."

        # Fresh veth for each run
        setup_ns

        # Start flood first, give it a moment
        start_flood "$DURATION"
        sleep 0.5

        # Run sink in the sink namespace
        out_file=$(mktemp)
        ip netns exec "$NS_SINK" "$BENCH_EXE" \
            --device "$config" \
            --ifname "$VETH_SINK" \
            --duration "$DURATION" \
            --json \
            > "$out_file" 2>/dev/null || true

        # Stop flood
        stop_flood

        # Check output
        if [ -s "$out_file" ] && python3 -m json.tool "$out_file" &>/dev/null; then
            if [ "$success" -gt 0 ]; then
                echo "," >> "$raw_file"
            fi
            cat "$out_file" >> "$raw_file"
            success=$((success + 1))

            mpps=$(python3 -c "import json; d=json.load(open('$out_file')); print(f\"{d['results']['mpps']:.4f}\")" 2>/dev/null || echo "?")
            echo "    -> ${mpps} Mpps"
        else
            echo "    -> FAILED"
            [ -s "$out_file" ] && cat "$out_file" >&2
        fi
        rm -f "$out_file"
    done

    echo "]" >> "$raw_file"
    echo "  Completed: $success/$NUM_RUNS runs"
    echo ""
done

# ─────────────────────────────────────────────────────────
# Post-processing
# ─────────────────────────────────────────────────────────
echo "--- Post-processing ---"

export RESULTS_DIR

python3 << 'PYEOF'
import json, os, statistics

results_dir = os.environ.get("RESULTS_DIR", ".")
summary = {"benchmark": "e2e_throughput", "configs": {}}

for fname in sorted(os.listdir(results_dir)):
    if not fname.endswith("_raw.json"):
        continue
    config = fname.replace("_raw.json", "")
    path = os.path.join(results_dir, fname)
    try:
        with open(path) as f:
            runs = json.load(f)
    except (json.JSONDecodeError, FileNotFoundError):
        continue
    if not runs:
        continue

    mpps_vals = [r["results"]["mpps"] for r in runs if "results" in r]
    gbps_vals = [r["results"]["gbps"] for r in runs if "results" in r]
    pps_vals  = [r["results"]["pps"]  for r in runs if "results" in r]

    if not mpps_vals:
        continue

    summary["configs"][config] = {
        "num_runs": len(mpps_vals),
        "mpps": {
            "mean": statistics.mean(mpps_vals),
            "std": statistics.stdev(mpps_vals) if len(mpps_vals) > 1 else 0,
            "values": mpps_vals,
        },
        "gbps": {
            "mean": statistics.mean(gbps_vals),
            "std": statistics.stdev(gbps_vals) if len(gbps_vals) > 1 else 0,
        },
        "pps": {"mean": statistics.mean(pps_vals)},
    }

out_path = os.path.join(results_dir, "summary.json")
with open(out_path, "w") as f:
    json.dump(summary, f, indent=2)
print(f"Summary: {out_path}")

# Print comparison
if len(summary["configs"]) >= 2:
    print("\n--- Comparison ---")
    for name, data in summary["configs"].items():
        m = data["mpps"]["mean"]
        s = data["mpps"]["std"]
        print(f"  {name:15s}: {m:.4f} +/- {s:.4f} Mpps")

    configs = list(summary["configs"].keys())
    if "raw_socket" in summary["configs"] and "af_xdp" in summary["configs"]:
        raw = summary["configs"]["raw_socket"]["mpps"]["mean"]
        xdp = summary["configs"]["af_xdp"]["mpps"]["mean"]
        if raw > 0:
            print(f"\n  AF_XDP speedup: {xdp/raw:.2f}x over raw_socket (TUN-equivalent)")
PYEOF

ln -sfn "$RESULTS_DIR" "${PROJECT_ROOT}/bench_results/latest_e2e"

echo ""
echo "============================================"
echo "  Results: $RESULTS_DIR"
echo "============================================"
