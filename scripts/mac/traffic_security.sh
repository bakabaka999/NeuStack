#!/bin/bash
# scripts/mac/traffic_security.sh
#
# 防火墙安全模型的流量生成器
#
# 生成两种流量模式:
#   normal  - 正常 HTTP 流量 (用于 label=0 训练)
#   attack  - 模拟攻击流量 (用于 label=1 训练)
#
# 攻击模式包括:
#   1. SYN Flood     - 大量快速连接尝试 (高 SYN rate)
#   2. Port Scan     - 连接随机端口 (高 RST rate)
#   3. Connection Flood - 大量并发连接
#   4. Slowloris     - 慢速请求占用连接
#   5. Request Flood - 大量 HTTP 小请求
#
# 用法:
#   bash scripts/mac/traffic_security.sh SERVER_IP [options]
#
# 选项:
#   --duration N    持续时间 (分钟, 默认: 10)
#   --http-port N   HTTP 端口 (默认: 本地80, 远程8080)
#   --mode MODE     normal|attack|mixed (默认: mixed)

set -e

# ─── 参数解析 ───
SERVER_IP=""
DURATION=10
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
    echo "  --duration N   Duration in minutes (default: 10)"
    echo "  --http-port N  HTTP port (default: auto)"
    echo "  --mode MODE    normal|attack|mixed (default: mixed)"
    echo ""
    echo "Modes:"
    echo "  normal  - Normal HTTP traffic (for label=0 collection)"
    echo "  attack  - Simulated attack patterns (for label=1 collection)"
    echo "  mixed   - Alternating normal and attack phases"
    exit 1
fi

if [ -z "$HTTP_PORT" ]; then
    if [[ "$SERVER_IP" == 192.168.* ]]; then
        HTTP_PORT=80
    else
        HTTP_PORT=8080
    fi
fi

BASE_URL="http://$SERVER_IP:$HTTP_PORT"

echo "=============================================="
echo "  NeuStack Security Traffic Generator"
echo "=============================================="
echo "  Server:   $SERVER_IP:$HTTP_PORT"
echo "  Duration: ${DURATION}m"
echo "  Mode:     $MODE"
echo "=============================================="
echo ""

# ─── 连通性测试 ───
echo "[*] Testing connectivity..."
if curl -s -m 5 "$BASE_URL/api/status" > /dev/null 2>&1; then
    echo "  ✓ HTTP OK"
else
    echo "  ✗ Cannot connect to $BASE_URL"
    exit 1
fi

# ============================================================================
# 正常流量函数
# ============================================================================

normal_browsing() {
    # 模拟正常用户浏览: 间隔大, 文件大小随机
    local rounds="$1"
    echo "  [Normal] Browsing pattern ($rounds requests)..."
    for i in $(seq 1 "$rounds"); do
        case $((RANDOM % 4)) in
            0) curl -s -m 30 -o /dev/null "$BASE_URL/" ;;
            1) curl -s -m 30 -o /dev/null "$BASE_URL/api/status" ;;
            2) curl -s -m 30 -o /dev/null "$BASE_URL/download/100k" ;;
            3) curl -s -m 60 -o /dev/null "$BASE_URL/download/1m" ;;
        esac
        # 正常用户间隔 0.5-3 秒
        sleep "$(python3 -c "import random; print(f'{random.uniform(0.5, 3.0):.1f}')" 2>/dev/null || echo 1)"
    done
}

normal_downloads() {
    # 正常下载: 少量并发, 合理间隔
    local rounds="$1"
    echo "  [Normal] Download pattern ($rounds rounds)..."
    for i in $(seq 1 "$rounds"); do
        local conns=$((RANDOM % 3 + 1))  # 1-3 并发
        for j in $(seq 1 "$conns"); do
            local sizes=("100k" "1m" "5m")
            local size=${sizes[$((RANDOM % ${#sizes[@]}))]}
            curl -s -m 120 -o /dev/null "$BASE_URL/download/$size" &
        done
        wait
        sleep "$((RANDOM % 3 + 1))"
    done
}

# ============================================================================
# 攻击流量函数
# ============================================================================

attack_syn_flood() {
    # SYN Flood: 大量快速短连接 → 高 SYN rate, 高 new_conn_rate
    local duration_sec="$1"
    echo "  [Attack] SYN Flood (${duration_sec}s)..."
    local end_time=$((SECONDS + duration_sec))
    while [ $SECONDS -lt $end_time ]; do
        # 20 个并发极短连接
        for i in $(seq 1 20); do
            curl -s -m 1 -o /dev/null "$BASE_URL/download/1k" 2>/dev/null &
        done
        wait
        # 不间隔, 尽可能快
    done
}

attack_port_scan() {
    # Port Scan: 连接不存在的端口 → 高 RST rate, 高 rst_ratio
    local duration_sec="$1"
    echo "  [Attack] Port Scan (${duration_sec}s)..."
    local end_time=$((SECONDS + duration_sec))
    while [ $SECONDS -lt $end_time ]; do
        for port in $(shuf -i 1-65535 -n 10 2>/dev/null || seq 1000 1010); do
            # 尝试连接随机端口, 大部分会被 RST
            (echo "" | nc -w 1 "$SERVER_IP" "$port" 2>/dev/null) &
        done
        wait
        sleep 0.1
    done
}

attack_connection_flood() {
    # Connection Flood: 大量并发 HTTP 请求 → 高 pps, 高 active_connections
    local duration_sec="$1"
    echo "  [Attack] Connection Flood (${duration_sec}s)..."
    local end_time=$((SECONDS + duration_sec))
    while [ $SECONDS -lt $end_time ]; do
        # 30+ 并发连接
        for i in $(seq 1 30); do
            curl -s -m 5 -o /dev/null "$BASE_URL/download/100k" 2>/dev/null &
        done
        wait
    done
}

attack_slowloris() {
    # Slowloris: 打开连接后发送极慢的数据 → 高 active_connections, 低 pps
    local duration_sec="$1"
    echo "  [Attack] Slowloris (${duration_sec}s)..."

    # 启动多个慢速连接
    local pids=()
    for i in $(seq 1 15); do
        (
            # 用 nc 发送不完整的 HTTP 请求, 每隔几秒发一点
            {
                echo -n "GET /download/10m HTTP/1.1\r\nHost: $SERVER_IP\r\n"
                local inner_end=$((SECONDS + duration_sec))
                while [ $SECONDS -lt $inner_end ]; do
                    echo -n "X-Padding-$RANDOM: $(head -c 16 /dev/urandom | base64)\r\n"
                    sleep "$((RANDOM % 3 + 2))"
                done
                echo -e "\r\n"
            } | nc -w "$((duration_sec + 5))" "$SERVER_IP" "$HTTP_PORT" 2>/dev/null
        ) &
        pids+=($!)
        sleep 0.2
    done

    # 等待 duration 结束
    sleep "$duration_sec"

    # 清理
    for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null
}

attack_request_flood() {
    # Request Flood: 大量极小请求 → 高 pps, 小 avg_pkt_size
    local duration_sec="$1"
    echo "  [Attack] Request Flood (${duration_sec}s)..."
    local end_time=$((SECONDS + duration_sec))
    while [ $SECONDS -lt $end_time ]; do
        for i in $(seq 1 50); do
            curl -s -m 2 -o /dev/null "$BASE_URL/api/status" 2>/dev/null &
        done
        wait
    done
}

# ============================================================================
# 主逻辑
# ============================================================================

TOTAL_SECONDS=$((DURATION * 60))

case "$MODE" in
    normal)
        echo ""
        echo "[*] Generating normal traffic for ${DURATION}m..."
        echo ""
        END_TIME=$((SECONDS + TOTAL_SECONDS))
        while [ $SECONDS -lt $END_TIME ]; do
            remaining=$(( (END_TIME - SECONDS) / 60 ))
            echo "  ~${remaining}m remaining"
            normal_browsing 10
            normal_downloads 3
        done
        ;;

    attack)
        echo ""
        echo "[*] Generating attack traffic for ${DURATION}m..."
        echo ""
        # 每种攻击轮流, 各 phase_sec 秒
        PHASE_SEC=$((TOTAL_SECONDS / 5))
        [ $PHASE_SEC -lt 10 ] && PHASE_SEC=10

        attack_syn_flood "$PHASE_SEC"
        attack_port_scan "$PHASE_SEC"
        attack_connection_flood "$PHASE_SEC"
        attack_slowloris "$PHASE_SEC"
        attack_request_flood "$PHASE_SEC"
        ;;

    mixed)
        echo ""
        echo "[*] Generating mixed traffic for ${DURATION}m..."
        echo "    (alternating 60s normal / 30s attack)"
        echo ""
        END_TIME=$((SECONDS + TOTAL_SECONDS))
        CYCLE=0
        while [ $SECONDS -lt $END_TIME ]; do
            CYCLE=$((CYCLE + 1))
            remaining=$(( (END_TIME - SECONDS) / 60 ))
            echo "━━━ Cycle $CYCLE (~${remaining}m remaining) ━━━"

            # 60s 正常流量
            echo "  Phase: Normal traffic (60s)"
            PHASE_END=$((SECONDS + 60))
            while [ $SECONDS -lt $PHASE_END ] && [ $SECONDS -lt $END_TIME ]; do
                normal_browsing 5
                normal_downloads 1
            done

            [ $SECONDS -ge $END_TIME ] && break

            # 30s 随机攻击
            ATTACK=$((RANDOM % 5))
            case $ATTACK in
                0) attack_syn_flood 30 ;;
                1) attack_port_scan 30 ;;
                2) attack_connection_flood 30 ;;
                3) attack_slowloris 30 ;;
                4) attack_request_flood 30 ;;
            esac
            echo ""
        done
        ;;

    *)
        echo "Unknown mode: $MODE (use normal|attack|mixed)"
        exit 1
        ;;
esac

echo ""
echo "=============================================="
echo "  Security Traffic Generation Complete!"
echo "=============================================="
echo ""
