#!/bin/bash
# scripts/mac/collect_security.sh
#
# 场景 7-8: macOS 本地安全数据采集
#
# 两阶段采集:
#   阶段 1: 正常流量 (label=0) — 另一终端跑 traffic_security.sh --mode normal
#   阶段 2: 攻击流量 (label=1) — 另一终端跑 traffic_security.sh --mode attack
#
# 或者一阶段混合采集 (仅 label=0, 攻击标注需手动):
#   traffic_security.sh --mode mixed  (此时全部标记为 label=0, 仅采集正常基线)
#
# 用法:
#   # 两阶段模式 (推荐, 产生标注数据)
#   sudo bash scripts/mac/collect_security.sh --phase normal --duration 10
#   sudo bash scripts/mac/collect_security.sh --phase attack --duration 10
#
#   # 一键全自动 (先 normal 后 attack, 需另一终端配合)
#   sudo bash scripts/mac/collect_security.sh --auto --duration 10

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ─── 参数解析 ───
PHASE="normal"      # normal | attack | auto
DURATION=10          # 每阶段分钟数
OUTPUT_DIR="$PROJECT_ROOT/collected_data"
NEUSTACK_IP="192.168.100.2"
HOST_IP="192.168.100.1"
AUTO_MODE=false

while [ $# -gt 0 ]; do
    case "$1" in
        --phase)      PHASE="$2"; shift 2;;
        --duration)   DURATION="$2"; shift 2;;
        --output-dir) OUTPUT_DIR="$2"; shift 2;;
        --ip)         NEUSTACK_IP="$2"; shift 2;;
        --auto)       AUTO_MODE=true; shift;;
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

# 确定 label
if [ "$PHASE" = "attack" ]; then
    SECURITY_LABEL=1
else
    SECURITY_LABEL=0
fi

echo "=============================================="
echo "  NeuStack macOS Security Data Collection"
echo "=============================================="
echo "  Phase:     $PHASE (label=$SECURITY_LABEL)"
echo "  NeuStack:  $NEUSTACK_IP"
echo "  Duration:  ${DURATION}m"
echo "  Output:    $OUTPUT_DIR"
echo "=============================================="
echo ""

# ─── 单阶段采集函数 ───

run_collection() {
    local label="$1"
    local phase_name="$2"
    local duration_min="$3"

    NEUSTACK_PID=""

    phase_cleanup() {
        echo ""
        echo "Stopping $phase_name collection..."
        [ -n "$NEUSTACK_PID" ] && kill $NEUSTACK_PID 2>/dev/null || true
        sleep 1

        # 重命名 security_data.csv
        local SRC="$OUTPUT_DIR/security_data.csv"
        if [ -f "$SRC" ]; then
            local DST="$OUTPUT_DIR/security_data_${phase_name}_${TIMESTAMP}.csv"
            mv "$SRC" "$DST"
            local LINES=$(wc -l < "$DST")
            echo "  $DST ($LINES lines)"
        fi

        # 也重命名 tcp/global (如果存在)
        for f in tcp_samples.csv global_metrics.csv; do
            local S="$OUTPUT_DIR/$f"
            if [ -f "$S" ]; then
                local BASE="${f%.csv}"
                local D="$OUTPUT_DIR/${BASE}_security_${phase_name}_${TIMESTAMP}.csv"
                mv "$S" "$D"
            fi
        done
    }

    echo "[1/2] Starting NeuStack (label=$label)..."
    cd "$PROJECT_ROOT/build/examples"
    ./neustack_demo --ip "$NEUSTACK_IP" \
        --collect --output-dir "$OUTPUT_DIR" \
        --security-collect --security-label "$label" &
    NEUSTACK_PID=$!
    sleep 2

    echo "[2/2] Configuring TUN..."
    UTUN_DEV=$(ifconfig -l | tr ' ' '\n' | grep utun | tail -1)
    if [ -z "$UTUN_DEV" ]; then
        sleep 2
        UTUN_DEV=$(ifconfig -l | tr ' ' '\n' | grep utun | tail -1)
    fi
    if [ -z "$UTUN_DEV" ]; then
        echo "ERROR: Cannot find utun device"
        phase_cleanup
        return 1
    fi
    ifconfig "$UTUN_DEV" "$HOST_IP" "$NEUSTACK_IP" up
    echo "  $UTUN_DEV: $HOST_IP <-> $NEUSTACK_IP"

    echo ""
    echo "=============================================="
    echo "  Ready! In another terminal run:"
    echo ""
    echo "  bash scripts/mac/traffic_security.sh $NEUSTACK_IP --duration $duration_min --mode $phase_name"
    echo ""
    echo "  Waiting for ${duration_min}m..."
    echo "=============================================="
    echo ""

    sleep "$((duration_min * 60))"

    phase_cleanup
}

# ─── 主逻辑 ───

if [ "$AUTO_MODE" = true ]; then
    echo "[Auto] Phase 1: Normal traffic (label=0)"
    echo ""
    run_collection 0 "normal" "$DURATION"
    echo ""
    echo "[Auto] Phase 2: Attack traffic (label=1)"
    echo ""
    run_collection 1 "attack" "$DURATION"
else
    cleanup() {
        echo ""
        echo "Stopping..."
        [ -n "$NEUSTACK_PID" ] && kill $NEUSTACK_PID 2>/dev/null || true
        sleep 1

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
        echo ""
        echo "Done! Data saved to: $OUTPUT_DIR"
    }
    trap cleanup EXIT

    echo "[1/2] Starting NeuStack (label=$SECURITY_LABEL)..."
    cd "$PROJECT_ROOT/build/examples"
    ./neustack_demo --ip "$NEUSTACK_IP" \
        --collect --output-dir "$OUTPUT_DIR" \
        --security-collect --security-label "$SECURITY_LABEL" &
    NEUSTACK_PID=$!
    sleep 2

    echo "[2/2] Configuring TUN..."
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

    echo ""
    echo "=============================================="
    echo "  Ready! In another terminal run:"
    echo ""
    if [ "$PHASE" = "attack" ]; then
        echo "  bash scripts/mac/traffic_security.sh $NEUSTACK_IP --duration $DURATION --mode attack"
    else
        echo "  bash scripts/mac/traffic_security.sh $NEUSTACK_IP --duration $DURATION --mode normal"
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
fi

echo ""
echo "Security data collection complete!"
