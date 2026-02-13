#!/bin/bash
# scripts/linux/traffic_anomaly.sh
#
# Anomaly 模型专用流量生成器 (Linux 客户端版)
#
# 生成多样化的连接模式，用于异常检测模型训练：
# - 短连接（频繁建立/断开）
# - 中断连接（产生 RST）
# - 并发连接波动
# - 混合长短连接
#
# 用法:
#   bash scripts/linux/traffic_anomaly.sh SERVER_IP [options]
#
# 选项:
#   --duration N    持续时间 (分钟, 默认: 15)
#   --http-port N   HTTP 端口 (默认: 本地用80, 远程用8080)
#   --mode MODE     模式: short, mixed, burst (默认: mixed)

set -e

# ─── 参数解析 ───
SERVER_IP=""
DURATION=15
HTTP_PORT=""
MODE="mixed"

while [ $# -gt 0 ]; do
    case "$1" in
        --duration)   DURATION="$2"; shift 2;;
        --http-port)  HTTP_PORT="$2"; shift 2;;
        --mode)       MODE="$2"; shift 2;;
        -*)           echo "Unknown: $1"; exit 1;;
        *)            SERVER_IP="$1"; shift;;
    esac
done

if [ -z "$SERVER_IP" ]; then
    echo "Usage: $0 SERVER_IP [options]"
    echo ""
    echo "Options:"
    echo "  --duration N   Duration in minutes (default: 15)"
    echo "  --http-port N  HTTP port (default: auto)"
    echo "  --mode MODE    short|mixed|burst (default: mixed)"
    echo ""
    echo "Modes:"
    echo "  short  - Many short-lived connections (frequent SYN/FIN)"
    echo "  mixed  - Mix of short and long connections"
    echo "  burst  - Bursty connection patterns with interruptions"
    exit 1
fi

# 自动检测端口
if [ -z "$HTTP_PORT" ]; then
    if [[ "$SERVER_IP" == 10.0.* ]] || [[ "$SERVER_IP" == 192.168.* ]]; then
        HTTP_PORT=80
    else
        HTTP_PORT=8080
    fi
fi

BASE_URL="http://$SERVER_IP:$HTTP_PORT"

echo "=============================================="
echo "  NeuStack Anomaly Traffic Generator (Linux)"
echo "=============================================="
echo "  Server:   $SERVER_IP:$HTTP_PORT"
echo "  Duration: ${DURATION}m"
echo "  Mode:     $MODE"
echo "=============================================="
echo ""

# ─── 连通性测试 ───
echo "[1/2] Testing connectivity..."
if curl -s -m 5 "$BASE_URL/api/status" > /dev/null 2>&1; then
    echo "  ✓ HTTP OK"
else
    echo "  ✗ Cannot connect to $BASE_URL"
    exit 1
fi

# ─── 流量生成函数 ───

short_connection() {
    curl -s -m 3 -o /dev/null "$BASE_URL/download/1k" 2>/dev/null || true
}

medium_connection() {
    curl -s -m 15 -o /dev/null "$BASE_URL/download/100k" 2>/dev/null || true
}

long_connection() {
    curl -s -m 60 -o /dev/null "$BASE_URL/download/5m" 2>/dev/null || true
}

interrupted_connection() {
    timeout 0.3 curl -s -o /dev/null "$BASE_URL/download/10m" 2>/dev/null || true
}

burst_short_connections() {
    local count="$1"
    for i in $(seq 1 $count); do
        short_connection &
    done
    wait
}

# ─── 主流量生成循环 ───
echo "[2/2] Generating traffic (mode: $MODE)..."
echo ""

END_TIME=$((SECONDS + DURATION * 60))
CYCLE=0

case "$MODE" in
    short)
        echo "  Generating short-lived connections..."
        while [ $SECONDS -lt $END_TIME ]; do
            CYCLE=$((CYCLE + 1))

            for i in $(seq 1 10); do
                short_connection &
            done
            wait

            if [ $((CYCLE % 5)) -eq 0 ]; then
                interrupted_connection
            fi

            sleep 0.2

            if [ $((CYCLE % 50)) -eq 0 ]; then
                echo "    Cycle $CYCLE: $((END_TIME - SECONDS))s remaining"
            fi
        done
        ;;

    mixed)
        echo "  Generating mixed connection patterns..."
        while [ $SECONDS -lt $END_TIME ]; do
            CYCLE=$((CYCLE + 1))

            # 阶段 1: 短连接
            echo -n "."
            burst_short_connections 5
            sleep 0.5

            # 阶段 2: 中等连接
            for i in $(seq 1 3); do
                medium_connection &
            done
            wait

            # 阶段 3: 长连接 + 背景短连接
            long_connection &
            LONG_PID=$!

            for i in $(seq 1 5); do
                burst_short_connections 3
                sleep 1
            done

            kill $LONG_PID 2>/dev/null || true
            wait $LONG_PID 2>/dev/null || true

            # 阶段 4: 中断连接
            if [ $((CYCLE % 3)) -eq 0 ]; then
                echo -n "x"
                for i in $(seq 1 3); do
                    interrupted_connection &
                done
                wait
            fi

            if [ $((CYCLE % 5)) -eq 0 ]; then
                echo ""
                echo "    Cycle $CYCLE: $((END_TIME - SECONDS))s remaining"
            fi
        done
        ;;

    burst)
        echo "  Generating bursty connection patterns..."
        while [ $SECONDS -lt $END_TIME ]; do
            CYCLE=$((CYCLE + 1))

            # 静默期
            QUIET_TIME=$((RANDOM % 4 + 2))
            echo "    Quiet period: ${QUIET_TIME}s"
            sleep $QUIET_TIME

            # 突发期
            BURST_SIZE=$((RANDOM % 15 + 5))
            echo "    Burst: $BURST_SIZE connections"

            for i in $(seq 1 $BURST_SIZE); do
                case $((RANDOM % 4)) in
                    0) short_connection & ;;
                    1) medium_connection & ;;
                    2) interrupted_connection & ;;
                    3) short_connection & ;;
                esac
            done
            wait

            if [ $((CYCLE % 4)) -eq 0 ]; then
                echo "    Long connection in background"
                long_connection &
            fi

            echo "    Cycle $CYCLE: $((END_TIME - SECONDS))s remaining"
        done
        wait
        ;;
esac

echo ""
echo ""
echo "=============================================="
echo "  Anomaly Traffic Generation Complete!"
echo "=============================================="
echo ""
echo "  This traffic pattern generates:"
echo "    - Frequent SYN (connection establishments)"
echo "    - Some RST (interrupted connections)"
echo "    - Variable active_connections"
echo ""
echo "  Check server data:"
echo "    tail -20 collected_data/global_metrics.csv"
echo ""
