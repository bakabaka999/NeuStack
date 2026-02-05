#!/bin/bash
# scripts/linux/collect_bwvar.sh
#
# 场景3: 带宽突变采集 (在服务器上运行)
#
# 在 collect.sh 的基础上，额外用 tc 动态改变网络条件
# Mac 端照常跑 traffic.sh 发流量即可
#
# 用法:
#   sudo bash scripts/linux/collect_bwvar.sh [options]
#
# 选项:
#   --iface IFACE    网卡名 (默认: 自动检测)
#   --rounds N       循环轮数 (默认: 5, 每轮约2分钟)
#   --output-dir DIR 数据输出目录 (默认: collected_data/)
#   --ip IP          NeuStack IP (默认: 10.0.1.2)
#   --http-port N    HTTP 转发端口 (默认: 8080)
#   --echo-port N    TCP Echo 转发端口 (默认: 8007)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
IFACE=""
ROUNDS=5
PHASE_DURATION=30
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
NEUSTACK_IP="10.0.1.2"
HOST_IP="10.0.1.1"
HTTP_PORT=8080
ECHO_PORT=8007

while [ $# -gt 0 ]; do
    case "$1" in
        --iface)      IFACE="$2"; shift 2;;
        --rounds)     ROUNDS="$2"; shift 2;;
        --output-dir) OUTPUT_DIR="$2"; shift 2;;
        --ip)         NEUSTACK_IP="$2"; shift 2;;
        --http-port)  HTTP_PORT="$2"; shift 2;;
        --echo-port)  ECHO_PORT="$2"; shift 2;;
        *)            echo "Unknown: $1"; exit 1;;
    esac
done

HOST_IP=$(echo "$NEUSTACK_IP" | sed 's/\.[0-9]*$/.1/')

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo)"
    exit 1
fi

if [ ! -f "$PROJECT_ROOT/build/neustack" ]; then
    echo "ERROR: neustack binary not found"
    echo "  Run: sudo bash scripts/linux/setup.sh"
    exit 1
fi

# 自动检测网卡
if [ -z "$IFACE" ]; then
    IFACE=$(ip route | grep default | awk '{print $5}' | head -1)
fi

if [ -z "$IFACE" ] || ! ip link show "$IFACE" &>/dev/null; then
    echo "ERROR: Cannot detect network interface"
    echo "  Specify with: --iface eth0"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SERVER_IP=$(hostname -I | awk '{print $1}')
TOTAL_TIME=$((ROUNDS * PHASE_DURATION * 4))

echo "=============================================="
echo "  NeuStack Bandwidth Variation Collection"
echo "=============================================="
echo "  Server IP:     $SERVER_IP"
echo "  NeuStack IP:   $NEUSTACK_IP"
echo "  Interface:     $IFACE"
echo "  Rounds:        $ROUNDS"
echo "  Total time:    ~${TOTAL_TIME}s (~$((TOTAL_TIME/60))min)"
echo "  HTTP forward:  :$HTTP_PORT -> $NEUSTACK_IP:80"
echo "  Echo forward:  :$ECHO_PORT -> $NEUSTACK_IP:7"
echo "  Output:        $OUTPUT_DIR"
echo "=============================================="
echo ""

# ─── PID 追踪 ───
PIDS=()

cleanup() {
    echo ""
    echo "Stopping..."

    # 清理 tc
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    echo "  tc rules cleared"

    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 1

    # 重命名带时间戳
    for f in tcp_samples.csv global_metrics.csv; do
        SRC="$OUTPUT_DIR/$f"
        if [ -f "$SRC" ]; then
            BASE="${f%.csv}"
            DST="$OUTPUT_DIR/${BASE}_bwvar_${TIMESTAMP}.csv"
            mv "$SRC" "$DST"
            LINES=$(wc -l < "$DST")
            echo "  $DST ($LINES lines)"
        fi
    done

    ip link set tun0 down 2>/dev/null || true
    echo ""
    echo "Done! Data saved to: $OUTPUT_DIR"
}
trap cleanup EXIT INT TERM

# ─── 1. 启动 NeuStack ───
echo "[1/4] Starting NeuStack..."
cd "$PROJECT_ROOT/build"
./neustack --ip "$NEUSTACK_IP" --collect --output-dir "$OUTPUT_DIR" -v &
PIDS+=($!)
sleep 2

# ─── 2. 配置 TUN ───
echo "[2/4] Configuring TUN device..."
TUN_DEV=$(ip -o link show type tun 2>/dev/null | awk -F': ' '{print $2}' | head -1)
if [ -z "$TUN_DEV" ]; then
    TUN_DEV="tun0"
fi
ip addr add "$HOST_IP/24" dev "$TUN_DEV" 2>/dev/null || true
ip link set "$TUN_DEV" up
echo "  $TUN_DEV: $HOST_IP <-> $NEUSTACK_IP"

# ─── 3. 启动端口转发 ───
echo "[3/4] Starting port forwarding..."
socat TCP-LISTEN:$HTTP_PORT,fork,reuseaddr TCP:$NEUSTACK_IP:80 &
PIDS+=($!)
echo "  :$HTTP_PORT -> $NEUSTACK_IP:80 (HTTP)"

socat TCP-LISTEN:$ECHO_PORT,fork,reuseaddr TCP:$NEUSTACK_IP:7 &
PIDS+=($!)
echo "  :$ECHO_PORT -> $NEUSTACK_IP:7 (TCP Echo)"

# ─── 就绪 ───
echo ""
echo "=============================================="
echo "  Ready! Now run on Mac:"
echo ""
echo "  bash scripts/mac/traffic.sh $SERVER_IP --duration $((TOTAL_TIME / 60 + 1)) --mode normal"
echo ""
echo "=============================================="
echo ""

# ─── 4. tc 带宽变化循环 ───
echo "[4/4] Starting bandwidth variation..."
echo ""

for round in $(seq 1 $ROUNDS); do
    echo "━━━ Round $round/$ROUNDS ━━━"

    echo "  [$(date +%H:%M:%S)] Phase 1: Normal (no limit) - ${PHASE_DURATION}s"
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    sleep $PHASE_DURATION

    echo "  [$(date +%H:%M:%S)] Phase 2: 1Mbit + 50ms delay - ${PHASE_DURATION}s"
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    tc qdisc add dev "$IFACE" root handle 1: netem delay 50ms 10ms
    tc qdisc add dev "$IFACE" parent 1:1 tbf rate 1mbit burst 32kbit latency 200ms
    sleep $PHASE_DURATION

    echo "  [$(date +%H:%M:%S)] Phase 3: 5% loss + 20ms delay - ${PHASE_DURATION}s"
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    tc qdisc add dev "$IFACE" root netem loss 5% delay 20ms 5ms
    sleep $PHASE_DURATION

    echo "  [$(date +%H:%M:%S)] Phase 4: 100ms + jitter - ${PHASE_DURATION}s"
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    tc qdisc add dev "$IFACE" root netem delay 100ms 30ms distribution normal
    sleep $PHASE_DURATION

    echo ""
done

# cleanup 由 trap 自动调用
