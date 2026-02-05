#!/bin/bash
# scripts/mac/collect.sh
#
# 场景1: macOS 本地数据采集（流畅网络）
#
# NeuStack 通过 TUN 设备直连，无真实网络延迟
# 用于采集理想网络条件下的拥塞控制行为
#
# 用法:
#   sudo bash scripts/mac/collect.sh [options]
#
# 选项:
#   --duration N     采集时长 (分钟, 默认: 10)
#   --output-dir DIR 数据输出目录 (默认: collected_data/)
#   --mode MODE      流量模式: quick, normal, heavy (默认: normal)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
DURATION=10
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
MODE="normal"
NEUSTACK_IP="192.168.100.2"
HOST_IP="192.168.100.1"

while [ $# -gt 0 ]; do
    case "$1" in
        --duration)   DURATION="$2"; shift 2;;
        --output-dir) OUTPUT_DIR="$2"; shift 2;;
        --mode)       MODE="$2"; shift 2;;
        *)            echo "Unknown: $1"; exit 1;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo)"
    exit 1
fi

if [ ! -f "$PROJECT_ROOT/build/neustack" ]; then
    echo "ERROR: neustack binary not found"
    echo "  Run: bash scripts/mac/setup.sh"
    exit 1
fi

# 模式配置
case "$MODE" in
    quick)
        DOWNLOAD_SIZES=("1m")
        DOWNLOAD_ROUNDS=5
        PARALLEL_CONNS=(2 3)
        ;;
    normal)
        DOWNLOAD_SIZES=("1m" "5m" "10m")
        DOWNLOAD_ROUNDS=15
        PARALLEL_CONNS=(2 4 6)
        ;;
    heavy)
        DOWNLOAD_SIZES=("1m" "5m" "10m")
        DOWNLOAD_ROUNDS=30
        PARALLEL_CONNS=(4 8 12)
        ;;
    *)
        echo "Unknown mode: $MODE (use quick|normal|heavy)"
        exit 1
        ;;
esac

mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "=============================================="
echo "  NeuStack macOS Local Collection"
echo "=============================================="
echo "  Scenario:  Local (low latency TUN)"
echo "  NeuStack:  $NEUSTACK_IP"
echo "  Duration:  ${DURATION}m"
echo "  Mode:      $MODE"
echo "  Output:    $OUTPUT_DIR"
echo "=============================================="
echo ""

# ─── PID 追踪 ───
NEUSTACK_PID=""

cleanup() {
    echo ""
    echo "Stopping..."
    [ -n "$NEUSTACK_PID" ] && kill $NEUSTACK_PID 2>/dev/null || true
    sleep 1

    # 重命名带时间戳
    for f in tcp_samples.csv global_metrics.csv; do
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
echo "[1/3] Starting NeuStack..."
cd "$PROJECT_ROOT/build"
./neustack --ip "$NEUSTACK_IP" --collect --output-dir "$OUTPUT_DIR" -v &
NEUSTACK_PID=$!
sleep 2

# ─── 2. 配置 TUN ───
echo "[2/3] Configuring TUN device..."

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

# 等待设备就绪
sleep 1

# 验证连通性
echo "  Testing connectivity..."
if curl -s -m 3 "http://$NEUSTACK_IP/api/status" > /dev/null 2>&1; then
    echo "  ✓ HTTP OK"
else
    echo "  ✗ Cannot connect to NeuStack"
    exit 1
fi

# ─── 3. 生成流量 ───
echo "[3/3] Generating traffic..."
echo ""

# Phase 1: 串行下载
echo "  [Phase 1] Serial downloads (mixed sizes)..."
for i in $(seq 1 $DOWNLOAD_ROUNDS); do
    SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}
    curl -s -o /dev/null "http://$NEUSTACK_IP/download/$SIZE"
    [ $((i % 5)) -eq 0 ] && echo "    [$i/$DOWNLOAD_ROUNDS]"
done

# Phase 2: 并行下载
echo "  [Phase 2] Parallel downloads..."
for conns in "${PARALLEL_CONNS[@]}"; do
    echo "    $conns connections..."
    for round in $(seq 1 3); do
        for j in $(seq 1 $conns); do
            SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}
            curl -s -o /dev/null "http://$NEUSTACK_IP/download/$SIZE" &
        done
        wait
    done
done

# Phase 3: 持续负载
echo "  [Phase 3] Sustained load..."
SUSTAINED_DURATION=$((DURATION * 60 / 2))
END_TIME=$((SECONDS + SUSTAINED_DURATION))
while [ $SECONDS -lt $END_TIME ]; do
    CONNS=$((RANDOM % 6 + 1))
    for j in $(seq 1 $CONNS); do
        SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}
        curl -s -o /dev/null "http://$NEUSTACK_IP/download/$SIZE" &
    done
    wait
    echo -n "."
done
echo ""

echo ""
echo "  ✓ Traffic generation complete"

# cleanup 由 trap 自动调用
