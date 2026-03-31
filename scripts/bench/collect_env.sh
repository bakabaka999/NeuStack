#!/usr/bin/env bash
# collect_env.sh — 收集 benchmark 测试环境信息
# Usage: bash scripts/bench/collect_env.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "=== NeuStack Benchmark Environment ==="
echo ""

# Date
echo "[Date]"
echo "  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""

# Kernel
echo "[Kernel]"
echo "  $(uname -srm)"
echo ""

# CPU
echo "[CPU]"
if [ -f /proc/cpuinfo ]; then
    model=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || echo "unknown")
    cores=$(grep -c '^processor' /proc/cpuinfo 2>/dev/null || echo "unknown")
    echo "  Model:  $model"
    echo "  Cores:  $cores"
else
    echo "  $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo 'unknown')"
fi

# CPU frequency & governor (Linux only)
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "N/A")
    cur_freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo "N/A")
    max_freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || echo "N/A")
    echo "  Governor:  $gov"
    echo "  Current:   ${cur_freq} kHz"
    echo "  Max:       ${max_freq} kHz"
fi
echo ""

# Memory
echo "[Memory]"
if [ -f /proc/meminfo ]; then
    mem_total=$(grep MemTotal /proc/meminfo | awk '{print $2, $3}')
    echo "  Total: $mem_total"
else
    echo "  $(sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.0f GB", $1/1024/1024/1024}' || echo 'unknown')"
fi

# Hugepages
if [ -f /proc/meminfo ]; then
    hp_total=$(grep HugePages_Total /proc/meminfo 2>/dev/null | awk '{print $2}' || echo "0")
    hp_size=$(grep Hugepagesize /proc/meminfo 2>/dev/null | awk '{print $2, $3}' || echo "N/A")
    echo "  HugePages: $hp_total x $hp_size"
fi
echo ""

# NIC info (Linux only, best-effort)
echo "[Network]"
default_if=$(ip route 2>/dev/null | grep default | head -1 | awk '{print $5}' || echo "")
if [ -n "$default_if" ]; then
    driver=$(ethtool -i "$default_if" 2>/dev/null | grep driver | awk '{print $2}' || echo "N/A")
    speed=$(ethtool "$default_if" 2>/dev/null | grep Speed | awk '{print $2}' || echo "N/A")
    echo "  Interface: $default_if"
    echo "  Driver:    $driver"
    echo "  Speed:     $speed"
else
    echo "  (no default interface detected)"
fi
echo ""

# Git info
echo "[NeuStack Build]"
if command -v git &>/dev/null && [ -d "$PROJECT_ROOT/.git" ]; then
    echo "  Branch:  $(git -C "$PROJECT_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'N/A')"
    echo "  Commit:  $(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo 'N/A')"
    echo "  Dirty:   $(git -C "$PROJECT_ROOT" diff --quiet 2>/dev/null && echo 'no' || echo 'yes')"
fi

# Compiler
if command -v g++ &>/dev/null; then
    echo "  g++:     $(g++ --version 2>/dev/null | head -1)"
elif command -v clang++ &>/dev/null; then
    echo "  clang++: $(clang++ --version 2>/dev/null | head -1)"
fi

# AF_XDP compile status
afxdp_status="disabled"
if [ -f "$PROJECT_ROOT/build/CMakeCache.txt" ]; then
    if grep -q 'NEUSTACK_ENABLE_AF_XDP:BOOL=ON' "$PROJECT_ROOT/build/CMakeCache.txt" 2>/dev/null; then
        afxdp_status="enabled"
    fi
fi
echo "  AF_XDP:  $afxdp_status"
echo ""

echo "=== End Environment ==="
