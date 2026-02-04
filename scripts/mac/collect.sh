#!/bin/bash
# scripts/mac/collect.sh
#
# macOS 本地数据采集
#
# 用法:
#   sudo bash scripts/mac/collect.sh [output_dir]
#
# 示例:
#   sudo bash scripts/mac/collect.sh
#   sudo bash scripts/mac/collect.sh /tmp/neustack_data

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="${1:-$PROJECT_ROOT/collected_data}"
NEUSTACK_IP="192.168.100.2"
HOST_IP="192.168.100.1"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo)"
    exit 1
fi

echo "=============================================="
echo "  NeuStack macOS Data Collection"
echo "=============================================="
echo "  Output: $OUTPUT_DIR"
echo "=============================================="
echo ""

mkdir -p "$OUTPUT_DIR"

# 时间戳文件名
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# 清理函数
cleanup() {
    echo ""
    echo "Stopping..."
    [ -n "$NEUSTACK_PID" ] && kill $NEUSTACK_PID 2>/dev/null || true

    # 统计
    if [ -f "$OUTPUT_DIR/tcp_samples.csv" ]; then
        LINES=$(wc -l < "$OUTPUT_DIR/tcp_samples.csv")
        echo "  TCP samples:    $LINES lines"
        # 重命名加时间戳
        mv "$OUTPUT_DIR/tcp_samples.csv" "$OUTPUT_DIR/tcp_samples_${TIMESTAMP}.csv"
    fi
    if [ -f "$OUTPUT_DIR/global_metrics.csv" ]; then
        LINES=$(wc -l < "$OUTPUT_DIR/global_metrics.csv")
        echo "  Global metrics: $LINES lines"
        mv "$OUTPUT_DIR/global_metrics.csv" "$OUTPUT_DIR/global_metrics_${TIMESTAMP}.csv"
    fi
    echo ""
    echo "Files saved to: $OUTPUT_DIR"
}
trap cleanup EXIT

# 启动 NeuStack
echo "Starting NeuStack..."
cd "$PROJECT_ROOT/build"
./neustack --ip "$NEUSTACK_IP" --collect --output-dir "$OUTPUT_DIR" -v &
NEUSTACK_PID=$!
sleep 2

# 获取 utun 设备名
UTUN_DEV=$(./neustack --help 2>&1 | head -1 || true)  # 占位，实际从日志解析
# 简单方法：查找最新的 utun
UTUN_DEV=$(ifconfig -l | tr ' ' '\n' | grep utun | tail -1)

if [ -z "$UTUN_DEV" ]; then
    echo "Waiting for utun device..."
    sleep 2
    UTUN_DEV=$(ifconfig -l | tr ' ' '\n' | grep utun | tail -1)
fi

echo "Configuring $UTUN_DEV..."
ifconfig "$UTUN_DEV" "$HOST_IP" "$NEUSTACK_IP" up

echo ""
echo "=============================================="
echo "  Ready! Test commands:"
echo ""
echo "  # 基础测试"
echo "  ping $NEUSTACK_IP"
echo "  curl http://$NEUSTACK_IP/"
echo ""
echo "  # 生成流量"
echo "  for i in \$(seq 1 100); do curl -s http://$NEUSTACK_IP/api/status; done"
echo "  yes test | head -c 1000000 | nc $NEUSTACK_IP 7"
echo ""
echo "  Press Ctrl+C to stop"
echo "=============================================="

# 等待
wait $NEUSTACK_PID
