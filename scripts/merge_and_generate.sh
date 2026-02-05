#!/bin/bash
# scripts/merge_and_generate.sh
#
# 合并多场景数据并生成训练集
#
# 用法:
#   bash scripts/merge_and_generate.sh [data_dir]
#
# 数据目录应包含:
#   tcp_samples_*.csv (TCP 样本)
#   global_metrics_*.csv (可选，用于异常检测)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 数据目录 (默认 collected_data，也支持 data)
DATA_DIR="${1:-$PROJECT_ROOT/collected_data}"
if [ ! -d "$DATA_DIR" ]; then
    DATA_DIR="$PROJECT_ROOT/data"
fi

OUTPUT_DIR="$PROJECT_ROOT/training/real_data"

echo "=============================================="
echo "  Merge & Generate Training Datasets"
echo "=============================================="
echo "  Data dir:   $DATA_DIR"
echo "  Output dir: $OUTPUT_DIR"
echo ""

# ─── 查找 TCP 样本 ───
TCP_FILES=()
for f in "$DATA_DIR"/tcp_samples*.csv; do
    [ -f "$f" ] && TCP_FILES+=("$f")
done

if [ ${#TCP_FILES[@]} -eq 0 ]; then
    echo "ERROR: No tcp_samples*.csv found in $DATA_DIR"
    exit 1
fi

echo "Found ${#TCP_FILES[@]} TCP sample file(s):"
TOTAL=0
for f in "${TCP_FILES[@]}"; do
    COUNT=$(($(wc -l < "$f") - 1))
    TOTAL=$((TOTAL + COUNT))
    echo "  $(basename "$f"): $COUNT samples"
done
echo "  Total: $TOTAL samples"
echo ""

# ─── 合并 TCP 样本 ───
COMBINED_TCP="$DATA_DIR/tcp_samples_combined.csv"
echo "[1/2] Merging TCP samples..."

head -1 "${TCP_FILES[0]}" > "$COMBINED_TCP"
for f in "${TCP_FILES[@]}"; do
    tail -n +2 "$f" >> "$COMBINED_TCP"
done
echo "  -> $COMBINED_TCP"

# ─── 查找 Global Metrics (可选) ───
GLOBAL_FILES=()
for f in "$DATA_DIR"/global_metrics*.csv; do
    [ -f "$f" ] && GLOBAL_FILES+=("$f")
done

GLOBAL_ARGS=""
if [ ${#GLOBAL_FILES[@]} -gt 0 ]; then
    COMBINED_GLOBAL="$DATA_DIR/global_metrics_combined.csv"
    echo ""
    echo "Found ${#GLOBAL_FILES[@]} global metrics file(s), merging..."
    head -1 "${GLOBAL_FILES[0]}" > "$COMBINED_GLOBAL"
    for f in "${GLOBAL_FILES[@]}"; do
        tail -n +2 "$f" >> "$COMBINED_GLOBAL"
    done
    echo "  -> $COMBINED_GLOBAL"
    GLOBAL_ARGS="--global-metrics $COMBINED_GLOBAL"
fi

# ─── 生成训练数据集 ───
echo ""
echo "[2/2] Generating training datasets..."
mkdir -p "$OUTPUT_DIR"

python3 "$SCRIPT_DIR/csv_to_dataset.py" \
    --tcp-samples "$COMBINED_TCP" \
    $GLOBAL_ARGS \
    --output-dir "$OUTPUT_DIR"

echo ""
echo "=============================================="
echo "  Done!"
echo "=============================================="
echo ""
echo "Datasets:"
ls -lh "$OUTPUT_DIR"/*.npz 2>/dev/null | awk '{print "  " $NF " (" $5 ")"}'
echo ""
echo "Train:"
echo "  cd training/orca && python train.py --data ../real_data/orca_dataset.npz"
