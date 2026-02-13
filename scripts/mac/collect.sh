#!/bin/bash
# scripts/mac/collect.sh
#
# 场景1: macOS 本地数据采集（流畅网络）
#
# 启动 NeuStack + 配置 TUN，流量由另一个终端运行 traffic.sh 生成
#
# 用法:
#   sudo bash scripts/mac/collect.sh [options]
#
# 选项:
#   --hours N        采集时长 (默认: 不限, Ctrl+C 停止)
#   --output-dir DIR 数据输出目录 (默认: collected_data/)
#   --ip IP          NeuStack IP (默认: 192.168.100.2)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
HOURS=0
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
NEUSTACK_IP="192.168.100.2"
HOST_IP="192.168.100.1"
SECURITY_LABEL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --hours)           HOURS="$2"; shift 2;;
        --output-dir)      OUTPUT_DIR="$2"; shift 2;;
        --ip)              NEUSTACK_IP="$2"; shift 2;;
        --security-label)  SECURITY_LABEL="$2"; shift 2;;
        *)                 echo "Unknown: $1"; exit 1;;
    esac
done

# 从 NeuStack IP 推导 Host IP
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

mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "=============================================="
echo "  NeuStack macOS Local Collection"
echo "=============================================="
echo "  Scenario:  Local (low latency TUN)"
echo "  NeuStack:  $NEUSTACK_IP"
echo "  Output:    $OUTPUT_DIR"
if [ $HOURS -gt 0 ]; then
    echo "  Duration:  ${HOURS}h"
else
    echo "  Duration:  unlimited (Ctrl+C to stop)"
fi
echo "=============================================="
echo ""

# ─── PID 追踪 ───
NEUSTACK_PID=""

cleanup() {
    echo ""
    echo "Stopping..."
    [ -n "$NEUSTACK_PID" ] && kill $NEUSTACK_PID 2>/dev/null || true
    sleep 1

    # 重命名带时间戳（三个 CSV 全处理）
    for f in tcp_samples.csv global_metrics.csv security_data.csv; do
        SRC="$OUTPUT_DIR/$f"
        if [ -f "$SRC" ]; then
            BASE="${f%.csv}"
            DST="$OUTPUT_DIR/${BASE}_local_${TIMESTAMP}.csv"
            mv "$SRC" "$DST"
            LINES=$(wc -l < "$DST")
            echo "  $DST ($LINES lines)"
        fi
    done
    echo ""
    echo "Done! Data saved to: $OUTPUT_DIR"
}
trap cleanup EXIT

# ─── 1. 启动 NeuStack ───
echo "[1/2] Starting NeuStack..."
cd "$PROJECT_ROOT/build/examples"
./neustack_demo --ip "$NEUSTACK_IP" \
    --collect --output-dir "$OUTPUT_DIR" \
    --security-collect --security-label "$SECURITY_LABEL" &
NEUSTACK_PID=$!
sleep 2

# ─── 2. 配置 TUN ───
echo "[2/2] Configuring TUN device..."

# 查找 utun 设备
UTUN_DEV=$(ifconfig -l | tr ' ' '\n' | grep utun | tail -1)
if [ -z "$UTUN_DEV" ]; then
    sleep 2
    UTUN_DEV=$(ifconfig -l | tr ' ' '\n' | grep utun | tail -1)
fi

if [ -z "$UTUN_DEV" ]; then
    echo "ERROR: Cannot find utun device"
    exit 1
fi

ifconfig "$UTUN_DEV" "$HOST_IP" "$NEUSTACK_IP" up
echo "  $UTUN_DEV: $HOST_IP <-> $NEUSTACK_IP"

# ─── 就绪 ───
echo ""
echo "=============================================="
echo "  Ready! In another terminal run:"
echo ""
echo "  # Quick test"
echo "  curl http://$NEUSTACK_IP/api/status"
echo ""
echo "  # Generate traffic"
echo "  bash scripts/mac/traffic.sh $NEUSTACK_IP --duration 10"
echo ""
echo "  Press Ctrl+C to stop collection"
echo "=============================================="
echo ""

# 等待
if [ $HOURS -gt 0 ]; then
    sleep $((HOURS * 3600))
else
    wait $NEUSTACK_PID
fi
