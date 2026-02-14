#!/bin/bash
# scripts/linux/collect_security.sh
#
# 场景 7-8: Linux 服务器安全数据采集
#
# 在服务器上运行 NeuStack + 防火墙 + SecurityExporter
# Mac 客户端通过 traffic_security.sh 生成正常/攻击流量
#
# 两阶段采集:
#   阶段 1: --phase normal  → label=0, Mac 端跑 traffic_security.sh --mode normal
#   阶段 2: --phase attack  → label=1, Mac 端跑 traffic_security.sh --mode attack
#
# 用法:
#   sudo bash scripts/linux/collect_security.sh --phase normal --duration 10
#   sudo bash scripts/linux/collect_security.sh --phase attack --duration 10

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
PHASE="normal"
DURATION=10
HTTP_PORT=8080
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
NEUSTACK_IP="10.0.1.2"
HOST_IP="10.0.1.1"

while [ $# -gt 0 ]; do
    case "$1" in
        --phase)      PHASE="$2"; shift 2;;
        --duration)   DURATION="$2"; shift 2;;
        --http-port)  HTTP_PORT="$2"; shift 2;;
        --output-dir) OUTPUT_DIR="$2"; shift 2;;
        --ip)         NEUSTACK_IP="$2"; shift 2;;
        *)            echo "Unknown: $1"; exit 1;;
    esac
done

HOST_IP=$(echo "$NEUSTACK_IP" | sed 's/\.[0-9]*$/.1/')

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo)"
    exit 1
fi

if [ ! -f "$PROJECT_ROOT/build/examples/neustack_demo" ]; then
    echo "ERROR: neustack_demo binary not found"
    echo "  Run: cmake -B build && cmake --build build"
    exit 1
fi

# 确定 label
if [ "$PHASE" = "attack" ]; then
    SECURITY_LABEL=1
else
    SECURITY_LABEL=0
fi

mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SERVER_IP=$(hostname -I | awk '{print $1}')

echo "=============================================="
echo "  NeuStack Linux Security Data Collection"
echo "=============================================="
echo "  Phase:       $PHASE (label=$SECURITY_LABEL)"
echo "  Server IP:   $SERVER_IP"
echo "  NeuStack IP: $NEUSTACK_IP"
echo "  HTTP fwd:    :$HTTP_PORT -> $NEUSTACK_IP:80"
echo "  Duration:    ${DURATION}m"
echo "  Output:      $OUTPUT_DIR"
echo "=============================================="
echo ""

# ─── PID 追踪 ───
PIDS=()

cleanup() {
    echo ""
    echo "Stopping..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 1

    # 重命名带时间戳
    SRC="$OUTPUT_DIR/security_data.csv"
    if [ -f "$SRC" ]; then
        DST="$OUTPUT_DIR/security_data_${PHASE}_${TIMESTAMP}.csv"
        mv "$SRC" "$DST"
        LINES=$(wc -l < "$DST")
        echo "  $DST ($LINES lines)"
    fi

    for f in tcp_samples.csv global_metrics.csv; do
        S="$OUTPUT_DIR/$f"
        if [ -f "$S" ]; then
            BASE="${f%.csv}"
            D="$OUTPUT_DIR/${BASE}_security_${PHASE}_${TIMESTAMP}.csv"
            mv "$S" "$D"
        fi
    done

    ip link set tun0 down 2>/dev/null || true
    echo ""
    echo "Done! Data saved to: $OUTPUT_DIR"
}
trap cleanup EXIT

# ─── 1. 启动 NeuStack ───
echo "[1/3] Starting NeuStack (label=$SECURITY_LABEL)..."
cd "$PROJECT_ROOT/build/examples"
./neustack_demo --ip "$NEUSTACK_IP" \
    --collect --output-dir "$OUTPUT_DIR" \
    --security-collect --security-label "$SECURITY_LABEL" &
PIDS+=($!)
sleep 2

# ─── 2. 配置 TUN ───
echo "[2/3] Configuring TUN device..."
TUN_DEV=$(ip -o link show type tun 2>/dev/null | awk -F': ' '{print $2}' | head -1)
if [ -z "$TUN_DEV" ]; then
    TUN_DEV="tun0"
fi
ip addr add "$HOST_IP/24" dev "$TUN_DEV" 2>/dev/null || true
ip link set "$TUN_DEV" up
echo "  $TUN_DEV: $HOST_IP <-> $NEUSTACK_IP"

# ─── 3. 端口转发 ───
echo "[3/3] Starting port forwarding..."
socat TCP-LISTEN:$HTTP_PORT,fork,reuseaddr TCP:$NEUSTACK_IP:80 &
PIDS+=($!)
echo "  :$HTTP_PORT -> $NEUSTACK_IP:80"

# ─── 就绪 ───
echo ""
echo "=============================================="
echo "  Ready! From your Mac run:"
echo ""
if [ "$PHASE" = "attack" ]; then
    echo "  bash scripts/mac/traffic_security.sh $SERVER_IP \\"
    echo "    --http-port $HTTP_PORT --duration $DURATION --mode attack"
else
    echo "  bash scripts/mac/traffic_security.sh $SERVER_IP \\"
    echo "    --http-port $HTTP_PORT --duration $DURATION --mode normal"
fi
echo ""
echo "  Press Ctrl+C to stop collection"
echo "=============================================="
echo ""

if [ $DURATION -gt 0 ]; then
    sleep $((DURATION * 60))
else
    while true; do sleep 86400; done
fi
