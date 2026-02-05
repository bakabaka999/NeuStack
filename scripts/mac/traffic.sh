#!/bin/bash
# scripts/mac/traffic.sh
#
# 从 Mac 向 Linux 服务器发送大量流量
# 用于生成真实网络环境下的训练数据
#
# 用法:
#   bash scripts/mac/traffic.sh SERVER_IP [options]
#
# 选项:
#   --duration N    持续时间 (分钟, 默认: 10)
#   --http-port N   HTTP 端口 (默认: 8080)
#   --echo-port N   TCP Echo 端口 (默认: 8007)
#   --mode MODE     模式: quick, normal, heavy (默认: normal)
#
# 示例:
#   bash scripts/mac/traffic.sh 1.2.3.4                    # 远程 (socat 转发)
#   bash scripts/mac/traffic.sh 1.2.3.4 --duration 30 --mode heavy
#   bash scripts/mac/traffic.sh 192.168.100.2 --local      # 本地 (直连 NeuStack)

set -e

# ─── 参数解析 ───
SERVER_IP=""
DURATION=10
HTTP_PORT=8080
ECHO_PORT=8007
MODE="normal"
LOCAL_MODE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --duration)   DURATION="$2"; shift 2;;
        --http-port)  HTTP_PORT="$2"; shift 2;;
        --echo-port)  ECHO_PORT="$2"; shift 2;;
        --mode)       MODE="$2"; shift 2;;
        --local)      LOCAL_MODE=1; shift;;
        -*)           echo "Unknown: $1"; exit 1;;
        *)            SERVER_IP="$1"; shift;;
    esac
done

if [ -z "$SERVER_IP" ]; then
    echo "Usage: $0 SERVER_IP [options]"
    echo ""
    echo "Options:"
    echo "  --duration N   Duration in minutes (default: 10)"
    echo "  --http-port N  HTTP port (default: 8080, or 80 for --local)"
    echo "  --echo-port N  Echo port (default: 8007, or 7 for --local)"
    echo "  --mode MODE    quick|normal|heavy (default: normal)"
    echo "  --local        Local mode (direct to NeuStack, no socat)"
    exit 1
fi

# 自动检测本地模式: 192.168.100.x
if [[ "$SERVER_IP" =~ ^192\.168\.100\. ]]; then
    LOCAL_MODE=1
fi

# 本地模式使用 NeuStack 直接端口
if [ $LOCAL_MODE -eq 1 ]; then
    [ $HTTP_PORT -eq 8080 ] && HTTP_PORT=80
    [ $ECHO_PORT -eq 8007 ] && ECHO_PORT=7
fi

# 模式配置
case "$MODE" in
    quick)
        HTTP_CONNS=(10 20)
        BULK_SIZES=(10 20)
        ;;
    normal)
        HTTP_CONNS=(10 30 50 100)
        BULK_SIZES=(10 50 100)
        ;;
    heavy)
        HTTP_CONNS=(50 100 200 500)
        BULK_SIZES=(50 100 200 500)
        ;;
    *)
        echo "Unknown mode: $MODE (use quick|normal|heavy)"
        exit 1
        ;;
esac

echo "=============================================="
echo "  NeuStack Traffic Generator"
echo "=============================================="
echo "  Server:   $SERVER_IP"
echo "  HTTP:     :$HTTP_PORT"
echo "  Echo:     :$ECHO_PORT"
echo "  Duration: ${DURATION}m"
echo "  Mode:     $MODE"
echo "=============================================="
echo ""

# ─── 检测工具 ───
HAS_WRK=0; command -v wrk &>/dev/null && HAS_WRK=1
HAS_AB=0;  command -v ab &>/dev/null && HAS_AB=1
HAS_NC=0;  command -v nc &>/dev/null && HAS_NC=1

echo "Tools available:"
[ $HAS_WRK -eq 1 ] && echo "  ✓ wrk"  || echo "  ✗ wrk (brew install wrk)"
[ $HAS_AB  -eq 1 ] && echo "  ✓ ab"   || echo "  ✗ ab"
[ $HAS_NC  -eq 1 ] && echo "  ✓ nc"   || echo "  ✗ nc"
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

# ─── 2. 预热 ───
echo "[2/5] Warming up..."
for i in {1..20}; do
    curl -s "http://$SERVER_IP:$HTTP_PORT/api/status" > /dev/null &
done
wait
echo "  Done"

# ─── 3. HTTP 并发压测 ───
echo "[3/5] HTTP stress test..."

# 每种并发度测试
PHASE_DURATION=$((DURATION * 60 / ${#HTTP_CONNS[@]} / 2))

for conns in "${HTTP_CONNS[@]}"; do
    echo "  Connections: $conns (${PHASE_DURATION}s)"
    if [ $HAS_WRK -eq 1 ]; then
        wrk -t4 -c$conns -d${PHASE_DURATION}s "http://$SERVER_IP:$HTTP_PORT/api/status" 2>&1 \
            | grep -E "Requests/sec|Latency|Transfer" | sed 's/^/    /' || true
    elif [ $HAS_AB -eq 1 ]; then
        ab -n $((conns * PHASE_DURATION * 10)) -c $conns -q "http://$SERVER_IP:$HTTP_PORT/api/status" 2>&1 \
            | grep -E "Requests per|Time per" | sed 's/^/    /' || true
    else
        END_TIME=$((SECONDS + PHASE_DURATION))
        while [ $SECONDS -lt $END_TIME ]; do
            for j in $(seq 1 $conns); do
                curl -s "http://$SERVER_IP:$HTTP_PORT/api/status" > /dev/null &
            done
            wait
        done
    fi
    sleep 2
done

# ─── 4. 大数据传输 ───
echo "[4/5] Bulk data transfer..."

if [ $HAS_NC -eq 1 ]; then
    for size in "${BULK_SIZES[@]}"; do
        echo "  Sending ${size}MB..."
        START=$(python3 -c 'import time; print(time.time())')
        dd if=/dev/urandom bs=1M count=$size 2>/dev/null | nc -w 60 "$SERVER_IP" "$ECHO_PORT" > /dev/null
        END=$(python3 -c 'import time; print(time.time())')
        SPEED=$(python3 -c "print(f'{$size / ($END - $START):.2f}')")
        echo "    Speed: ${SPEED} MB/s"
        sleep 1
    done
else
    echo "  SKIP: nc not available"
fi

# ─── 5. 混合流量 ───
echo "[5/5] Mixed traffic..."

# 同时跑 HTTP 和 TCP
MIXED_DURATION=60

echo "  Running HTTP + TCP in parallel (${MIXED_DURATION}s)..."

# 后台 HTTP
if [ $HAS_WRK -eq 1 ]; then
    wrk -t2 -c30 -d${MIXED_DURATION}s "http://$SERVER_IP:$HTTP_PORT/" > /dev/null 2>&1 &
elif [ $HAS_AB -eq 1 ]; then
    ab -n 10000 -c 30 -q "http://$SERVER_IP:$HTTP_PORT/" > /dev/null 2>&1 &
fi

# 后台 TCP
if [ $HAS_NC -eq 1 ]; then
    for i in 1 2 3; do
        dd if=/dev/urandom bs=1M count=100 2>/dev/null | nc -w 120 "$SERVER_IP" "$ECHO_PORT" > /dev/null &
    done
fi

wait
echo "  Done"

# ─── 完成 ───
echo ""
echo "=============================================="
echo "  Traffic Generation Complete!"
echo "=============================================="
echo ""
echo "  Check server data:"
echo "    ssh your-server 'ls -la collected_data/'"
echo "    ssh your-server 'wc -l collected_data/*.csv'"
echo ""
