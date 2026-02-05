#!/bin/bash
# scripts/linux/collect.sh
#
# Linux 服务器数据采集
#
# 启动 NeuStack + TUN 配置 + HTTP 端口转发
# Mac 客户端通过 HTTP 下载触发 NeuStack 发送数据，产生拥塞控制训练样本
#
# 核心原理：
# - NeuStack 作为 HTTP 服务器，提供 /download/1m, /download/10m 等端点
# - Mac 用 curl 下载，NeuStack 发送数据，触发拥塞控制算法
# - 拥塞控制的 cwnd/ssthresh/delivery_rate 等指标被采集到 CSV
#
# 用法:
#   sudo bash scripts/linux/collect.sh [options]
#
# 选项:
#   --hours N        采集时长 (默认: 不限, Ctrl+C 停止)
#   --http-port N    HTTP 转发端口 (默认: 8080)
#   --output-dir DIR 数据输出目录 (默认: collected_data/)
#   --ip IP          NeuStack IP (默认: 10.0.1.2)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
HOURS=0
HTTP_PORT=8080
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
NEUSTACK_IP="10.0.1.2"
HOST_IP="10.0.1.1"

while [ $# -gt 0 ]; do
    case "$1" in
        --hours)      HOURS="$2"; shift 2;;
        --http-port)  HTTP_PORT="$2"; shift 2;;
        --output-dir) OUTPUT_DIR="$2"; shift 2;;
        --ip)         NEUSTACK_IP="$2"; shift 2;;
        *)            echo "Unknown: $1"; exit 1;;
    esac
done

# 从 NeuStack IP 推导 Host IP (同子网 .1)
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

mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SERVER_IP=$(hostname -I | awk '{print $1}')

echo "=============================================="
echo "  NeuStack Linux Data Collection"
echo "=============================================="
echo "  Server IP:     $SERVER_IP"
echo "  NeuStack IP:   $NEUSTACK_IP"
echo "  HTTP forward:  :$HTTP_PORT -> $NEUSTACK_IP:80"
echo "  Output:        $OUTPUT_DIR"
if [ $HOURS -gt 0 ]; then
    echo "  Duration:      ${HOURS}h"
else
    echo "  Duration:      unlimited (Ctrl+C to stop)"
fi
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

    # 等待 NeuStack 退出 (flush CSV)
    sleep 1

    # 重命名带时间戳
    for f in tcp_samples.csv global_metrics.csv; do
        SRC="$OUTPUT_DIR/$f"
        if [ -f "$SRC" ]; then
            BASE="${f%.csv}"
            DST="$OUTPUT_DIR/${BASE}_${TIMESTAMP}.csv"
            mv "$SRC" "$DST"
            LINES=$(wc -l < "$DST")
            echo "  $DST ($LINES lines)"
        fi
    done

    # 清理 TUN 设备
    ip link set tun0 down 2>/dev/null || true
    echo ""
    echo "Done! Data saved to: $OUTPUT_DIR"
}
trap cleanup EXIT

# ─── 1. 启动 NeuStack ───
echo "[1/3] Starting NeuStack..."
cd "$PROJECT_ROOT/build"
./neustack --ip "$NEUSTACK_IP" --collect --output-dir "$OUTPUT_DIR" -v &
PIDS+=($!)
sleep 2

# ─── 2. 配置 TUN ───
echo "[2/3] Configuring TUN device..."

# 找到 NeuStack 创建的 TUN 设备
TUN_DEV=$(ip -o link show type tun 2>/dev/null | awk -F': ' '{print $2}' | head -1)
if [ -z "$TUN_DEV" ]; then
    TUN_DEV="tun0"
fi

ip addr add "$HOST_IP/24" dev "$TUN_DEV" 2>/dev/null || true
ip link set "$TUN_DEV" up
echo "  $TUN_DEV: $HOST_IP <-> $NEUSTACK_IP"

# ─── 3. 启动端口转发 ───
echo "[3/3] Starting port forwarding..."

socat TCP-LISTEN:$HTTP_PORT,fork,reuseaddr TCP:$NEUSTACK_IP:80 &
PIDS+=($!)
echo "  :$HTTP_PORT -> $NEUSTACK_IP:80 (HTTP)"

# ─── 就绪 ───
echo ""
echo "=============================================="
echo "  Ready! From your Mac run:"
echo ""
echo "  # Quick test"
echo "  curl http://$SERVER_IP:$HTTP_PORT/api/status"
echo ""
echo "  # Auto traffic generation (recommended)"
echo "  bash scripts/mac/traffic.sh $SERVER_IP --http-port $HTTP_PORT"
echo ""
echo "  # Manual download test"
echo "  curl -o /dev/null http://$SERVER_IP:$HTTP_PORT/download/10m"
echo ""
echo "  Press Ctrl+C to stop collection"
echo "=============================================="
echo ""

# 等待
if [ $HOURS -gt 0 ]; then
    sleep $((HOURS * 3600))
else
    # 无限等待
    while true; do sleep 86400; done
fi
