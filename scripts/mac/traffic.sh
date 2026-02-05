#!/bin/bash
# scripts/mac/traffic.sh
#
# 向 NeuStack 请求 HTTP 下载，让 NeuStack 发送数据
# 用于生成拥塞控制训练数据
#
# 核心思路：用 HTTP 下载让 NeuStack 发送数据，触发拥塞控制
# - Discard 服务只收不发，无法采集拥塞控制数据
# - Echo 服务容易零窗口死锁
# - HTTP 下载：NeuStack 发送，客户端接收，不会死锁
#
# 用法:
#   bash scripts/mac/traffic.sh SERVER_IP [options]
#
# 选项:
#   --duration N    持续时间 (分钟, 默认: 10)
#   --http-port N   HTTP 端口 (默认: 本地192.168.x.x用80, 其他用8080)
#   --mode MODE     模式: quick, normal, heavy (默认: normal)
#
# 示例:
#   bash scripts/mac/traffic.sh 192.168.100.2              # 本地 TUN (端口 80)
#   bash scripts/mac/traffic.sh 1.2.3.4                    # 远程服务器 (端口 8080)
#   bash scripts/mac/traffic.sh 1.2.3.4 --duration 30 --mode heavy

set -e

# ─── 参数解析 ───
SERVER_IP=""
DURATION=10
HTTP_PORT=""  # 空表示自动检测
MODE="normal"

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
    echo "  --duration N   Duration in minutes (default: 10)"
    echo "  --http-port N  HTTP port (default: auto - 80 for local, 8080 for remote)"
    echo "  --mode MODE    quick|normal|heavy (default: normal)"
    exit 1
fi

# 自动检测端口：本地 TUN (192.168.x.x) 用 80，远程用 8080
if [ -z "$HTTP_PORT" ]; then
    if [[ "$SERVER_IP" == 192.168.* ]]; then
        HTTP_PORT=80
    else
        HTTP_PORT=8080
    fi
fi

# 模式配置
case "$MODE" in
    quick)
        DOWNLOAD_SIZES=("1m")
        DOWNLOAD_ROUNDS=5
        PARALLEL_CONNS=(2 3)
        BURST_ROUNDS=3
        ;;
    normal)
        DOWNLOAD_SIZES=("1m" "5m" "10m")
        DOWNLOAD_ROUNDS=15
        PARALLEL_CONNS=(2 4 6)
        BURST_ROUNDS=5
        ;;
    heavy)
        DOWNLOAD_SIZES=("1m" "5m" "10m")
        DOWNLOAD_ROUNDS=30
        PARALLEL_CONNS=(4 8 12)
        BURST_ROUNDS=10
        ;;
    *)
        echo "Unknown mode: $MODE (use quick|normal|heavy)"
        exit 1
        ;;
esac

echo "=============================================="
echo "  NeuStack Traffic Generator (Download Mode)"
echo "=============================================="
echo "  Server:   $SERVER_IP:$HTTP_PORT"
echo "  Duration: ${DURATION}m"
echo "  Mode:     $MODE"
echo "=============================================="
echo ""

# ─── 1. 连通性测试 ───
echo "[1/5] Testing connectivity..."
if curl -s -m 5 "http://$SERVER_IP:$HTTP_PORT/api/status" > /dev/null 2>&1; then
    echo "  ✓ HTTP OK"
else
    echo "  ✗ Cannot connect to http://$SERVER_IP:$HTTP_PORT"
    echo "  Make sure NeuStack is running on the server"
    exit 1
fi

# 测试下载端点（只检查响应头，不下载完整文件）
HTTP_CODE=$(curl -s -m 10 -o /dev/null -w "%{http_code}" "http://$SERVER_IP:$HTTP_PORT/download/1m" 2>/dev/null || echo "000")
if [ "$HTTP_CODE" = "200" ]; then
    echo "  ✓ Download endpoint OK"
elif [ "$HTTP_CODE" = "000" ]; then
    echo "  ✗ Download endpoint not available (connection failed)"
    echo "  Make sure NeuStack has /download/1m endpoint"
    exit 1
else
    echo "  ✗ Download endpoint returned HTTP $HTTP_CODE"
    exit 1
fi

# ─── 2. 串行下载（不同大小混合）───
echo "[2/5] Serial downloads (mixed sizes)..."
echo "  Testing different file sizes to vary transfer duration"

TOTAL_MB=0
for i in $(seq 1 $DOWNLOAD_ROUNDS); do
    # 随机选择下载大小
    SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}

    START=$(python3 -c 'import time; print(time.time())' 2>/dev/null || date +%s)
    curl -s -m 120 -o /dev/null "http://$SERVER_IP:$HTTP_PORT/download/$SIZE"
    END=$(python3 -c 'import time; print(time.time())' 2>/dev/null || date +%s)

    case "$SIZE" in
        1m)  TOTAL_MB=$((TOTAL_MB + 1)) ;;
        5m)  TOTAL_MB=$((TOTAL_MB + 5)) ;;
        10m) TOTAL_MB=$((TOTAL_MB + 10)) ;;
    esac

    if [ $((i % 5)) -eq 0 ]; then
        DURATION=$(python3 -c "print(f'{$END - $START:.2f}')" 2>/dev/null || echo "?")
        echo "    [$i/$DOWNLOAD_ROUNDS] ${SIZE} in ${DURATION}s"
    fi
done
echo "  ✓ Downloaded ${TOTAL_MB}MB total"

# ─── 3. 并行下载（多连接竞争）───
echo "[3/5] Parallel downloads (connection competition)..."
echo "  Multiple connections competing for bandwidth"

for conns in "${PARALLEL_CONNS[@]}"; do
    echo "  $conns parallel connections..."
    for round in $(seq 1 3); do
        for j in $(seq 1 $conns); do
            # 随机选择大小，制造不同完成时间
            SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}
            curl -s -m 120 -o /dev/null "http://$SERVER_IP:$HTTP_PORT/download/$SIZE" &
        done
        wait
        echo -n "."
    done
    echo ""
done
echo "  ✓ Done"

# ─── 4. 突发式请求（随机间隔）───
echo "[4/5] Burst traffic (random intervals)..."
echo "  Simulating real-world bursty traffic patterns"

for i in $(seq 1 $BURST_ROUNDS); do
    # 突发：快速连续发起多个请求
    BURST_SIZE=$((RANDOM % 5 + 2))  # 2-6 个并发
    echo "    Burst $i: $BURST_SIZE requests"

    for j in $(seq 1 $BURST_SIZE); do
        SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}
        curl -s -m 120 -o /dev/null "http://$SERVER_IP:$HTTP_PORT/download/$SIZE" &
    done
    wait

    # 随机间隔 0.5-3 秒
    SLEEP_TIME=$(python3 -c "import random; print(f'{random.uniform(0.5, 3):.1f}')" 2>/dev/null || echo "1")
    sleep "$SLEEP_TIME"
done
echo "  ✓ Done"

# ─── 5. 持续压力测试 ───
echo "[5/5] Sustained load (continuous downloads)..."

SUSTAINED_DURATION=$((DURATION * 60 / 3))
echo "  Running for ${SUSTAINED_DURATION}s with varying parallelism..."

END_TIME=$((SECONDS + SUSTAINED_DURATION))
while [ $SECONDS -lt $END_TIME ]; do
    # 随机并发数 1-6
    CONNS=$((RANDOM % 6 + 1))
    for j in $(seq 1 $CONNS); do
        SIZE=${DOWNLOAD_SIZES[$((RANDOM % ${#DOWNLOAD_SIZES[@]}))]}
        curl -s -m 120 -o /dev/null "http://$SERVER_IP:$HTTP_PORT/download/$SIZE" &
    done
    wait
    echo -n "."
done
echo ""
echo "  ✓ Done"

# ─── 完成 ───
echo ""
echo "=============================================="
echo "  Traffic Generation Complete!"
echo "=============================================="
echo ""
echo "  Check server data:"
echo "    ssh your-server 'wc -l collected_data/*.csv'"
echo ""
