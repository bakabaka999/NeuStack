#!/bin/bash
# scripts/mac/collect_local_tc.sh
#
# 场景2: macOS 本地数据采集 + 网络条件模拟
#
# 使用 dnctl/pfctl 模拟各种网络条件（延迟、丢包、带宽限制）
# 用于采集拥塞场景下的训练数据
#
# 用法:
#   sudo bash scripts/mac/collect_local_tc.sh [options]
#
# 选项:
#   --rounds N       循环轮数 (默认: 5)
#   --phase N        每个phase时长秒 (默认: 60)
#   --output-dir DIR 数据输出目录 (默认: collected_data/)
#   --ip IP          NeuStack IP (默认: 192.168.100.2)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
ROUNDS=5
PHASE_DURATION=60
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
NEUSTACK_IP="192.168.100.2"
HOST_IP="192.168.100.1"

while [ $# -gt 0 ]; do
    case "$1" in
        --rounds)     ROUNDS="$2"; shift 2;;
        --phase)      PHASE_DURATION="$2"; shift 2;;
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

mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# 计算总时间: 8 phases × PHASE_DURATION × ROUNDS
TOTAL_PHASES=8
TOTAL_TIME=$((ROUNDS * TOTAL_PHASES * PHASE_DURATION))

echo "=============================================="
echo "  NeuStack macOS Local Collection + TC"
echo "=============================================="
echo "  Scenario:    Local with network simulation"
echo "  NeuStack:    $NEUSTACK_IP"
echo "  Rounds:      $ROUNDS"
echo "  Phase time:  ${PHASE_DURATION}s"
echo "  Total time:  ~${TOTAL_TIME}s (~$((TOTAL_TIME/60))min)"
echo "  Output:      $OUTPUT_DIR"
echo "=============================================="
echo ""

# ─── PID 追踪 ───
NEUSTACK_PID=""
UTUN_DEV=""

cleanup() {
    echo ""
    echo "Stopping..."

    # 清理 pf 规则
    pfctl -f /etc/pf.conf 2>/dev/null || true
    dnctl -q flush 2>/dev/null || true
    echo "  Network simulation cleared"

    [ -n "$NEUSTACK_PID" ] && kill $NEUSTACK_PID 2>/dev/null || true
    sleep 1

    # 重命名带时间戳（三个 CSV 全处理）
    for f in tcp_samples.csv global_metrics.csv security_data.csv; do
        SRC="$OUTPUT_DIR/$f"
        if [ -f "$SRC" ]; then
            BASE="${f%.csv}"
            DST="$OUTPUT_DIR/${BASE}_local_tc_${TIMESTAMP}.csv"
            mv "$SRC" "$DST"
            LINES=$(wc -l < "$DST")
            echo "  $DST ($LINES lines)"
        fi
    done
    echo ""
    echo "Done! Data saved to: $OUTPUT_DIR"
}
trap cleanup EXIT INT TERM

# ─── 1. 启动 NeuStack ───
echo "[1/3] Starting NeuStack..."
cd "$PROJECT_ROOT/build/examples"
./neustack_demo --ip "$NEUSTACK_IP" \
    --collect --output-dir "$OUTPUT_DIR" \
    --security-collect --security-label 0 &
NEUSTACK_PID=$!
sleep 2

# ─── 2. 配置 TUN ───
echo "[2/3] Configuring TUN device..."
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

# ─── 辅助函数: 设置网络条件 ───
set_network_condition() {
    local delay_ms="$1"
    local loss_pct="$2"
    local bw_kbps="$3"
    local description="$4"

    # 清理旧规则
    pfctl -f /etc/pf.conf 2>/dev/null || true
    dnctl -q flush 2>/dev/null || true

    if [ "$delay_ms" = "0" ] && [ "$loss_pct" = "0" ] && [ "$bw_kbps" = "0" ]; then
        echo "  [$(date +%H:%M:%S)] $description - ${PHASE_DURATION}s"
        return
    fi

    # 构建 dnctl 规则
    local dnctl_opts=""
    [ "$delay_ms" != "0" ] && dnctl_opts="$dnctl_opts delay ${delay_ms}ms"
    [ "$loss_pct" != "0" ] && dnctl_opts="$dnctl_opts plr $(echo "scale=2; $loss_pct/100" | bc)"
    [ "$bw_kbps" != "0" ] && dnctl_opts="$dnctl_opts bw ${bw_kbps}Kbit/s"

    # 创建 dummynet pipe
    dnctl pipe 1 config $dnctl_opts

    # 创建 pf 规则文件
    local PF_RULES=$(mktemp)
    cat > "$PF_RULES" << EOF
dummynet-anchor "neustack"
anchor "neustack"
EOF

    local PF_ANCHOR=$(mktemp)
    cat > "$PF_ANCHOR" << EOF
dummynet in on $UTUN_DEV all pipe 1
dummynet out on $UTUN_DEV all pipe 1
EOF

    # 加载规则
    pfctl -f "$PF_RULES" 2>/dev/null || true
    pfctl -a neustack -f "$PF_ANCHOR" 2>/dev/null || true
    pfctl -e 2>/dev/null || true

    rm -f "$PF_RULES" "$PF_ANCHOR"

    echo "  [$(date +%H:%M:%S)] $description - ${PHASE_DURATION}s"
}

# ─── 就绪 ───
echo ""
echo "=============================================="
echo "  Ready! In another terminal run:"
echo ""
echo "  bash scripts/mac/traffic.sh $NEUSTACK_IP --duration $((TOTAL_TIME / 60 + 2)) --mode heavy"
echo ""
echo "=============================================="
echo ""

# ─── 3. 网络条件循环 ───
echo "[3/3] Starting network condition simulation..."
echo ""

for round in $(seq 1 $ROUNDS); do
    echo "━━━ Round $round/$ROUNDS ━━━"

    # Phase 1: 正常（无限制）
    set_network_condition 0 0 0 "Phase 1/8: Normal (no limit)"
    sleep $PHASE_DURATION

    # Phase 2: 轻微延迟
    set_network_condition 20 0 0 "Phase 2/8: 20ms delay"
    sleep $PHASE_DURATION

    # Phase 3: 中等延迟
    set_network_condition 50 0 0 "Phase 3/8: 50ms delay"
    sleep $PHASE_DURATION

    # Phase 4: 高延迟
    set_network_condition 100 0 0 "Phase 4/8: 100ms delay"
    sleep $PHASE_DURATION

    # Phase 5: 轻微丢包
    set_network_condition 20 1 0 "Phase 5/8: 20ms + 1% loss"
    sleep $PHASE_DURATION

    # Phase 6: 中等丢包
    set_network_condition 30 5 0 "Phase 6/8: 30ms + 5% loss"
    sleep $PHASE_DURATION

    # Phase 7: 带宽限制
    set_network_condition 20 0 1000 "Phase 7/8: 20ms + 1Mbps limit"
    sleep $PHASE_DURATION

    # Phase 8: 组合恶劣条件
    set_network_condition 100 3 512 "Phase 8/8: 100ms + 3% loss + 512Kbps"
    sleep $PHASE_DURATION

    echo ""
done

# cleanup 由 trap 自动调用
