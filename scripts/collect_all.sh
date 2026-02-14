#!/bin/bash
# scripts/collect_all.sh
#
# NeuStack 完整数据采集指南
#
# 需要采集 8 组数据用于训练四个 AI 模型:
#
# ┌─────────────────────────────────────────────────────────────────┐
# │  数据集          │ 用途              │ 采集方式                 │
# ├─────────────────────────────────────────────────────────────────┤
# │  1. 本机正常      │ Orca/BW 基线      │ Mac 本地 TUN             │
# │  2. 本机+TC      │ Orca/BW 拥塞      │ Mac 本地 + 网络模拟       │
# │  3. 服务器正常    │ Orca/BW 真实网络  │ Linux 服务器             │
# │  4. 服务器+TC    │ Orca/BW 极端条件  │ Linux 服务器 + tc        │
# │  5. Anomaly短连接 │ Anomaly 正常模式  │ 短连接流量模式           │
# │  6. Anomaly混合   │ Anomaly 正常模式  │ 混合连接流量模式          │
# │  7. Security正常  │ Security 基线     │ 防火墙 + 正常 HTTP       │
# │  8. Security攻击  │ Security 异常     │ 防火墙 + 模拟攻击        │
# └─────────────────────────────────────────────────────────────────┘
#
# 模型与数据对应:
#   - Orca (拥塞控制):     使用 1-4 组的 tcp_samples.csv
#   - Bandwidth (带宽预测): 使用 1-4 组的 tcp_samples.csv
#   - Anomaly (异常检测):   使用 1-6 组的 global_metrics.csv
#   - Security (安全检测):  使用 7-8 组的 security_data.csv

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cat << 'EOF'
╔══════════════════════════════════════════════════════════════════╗
║           NeuStack 完整数据采集指南                              ║
╚══════════════════════════════════════════════════════════════════╝

建议采集顺序和时长:
  1. 本机正常     ~15分钟  (基线数据)
  2. 本机+TC     ~30分钟  (8种网络条件)
  3. 服务器正常   ~15分钟  (真实网络)
  4. 服务器+TC   ~30分钟  (10种网络条件)
  5. Anomaly短连接 ~15分钟  (短连接模式)
  6. Anomaly混合  ~15分钟  (混合模式)
  7. Security正常 ~10分钟  (防火墙正常基线)
  8. Security攻击 ~10分钟  (模拟攻击)

总计: ~2.5小时

══════════════════════════════════════════════════════════════════

【场景 1】本机正常采集 (Mac)

  终端 1 - 启动 NeuStack:
    sudo bash scripts/mac/collect.sh --hours 0

  终端 2 - 生成流量:
    bash scripts/mac/traffic.sh 192.168.100.2 --duration 15 --mode heavy

  输出: collected_data/tcp_samples_local_*.csv
        collected_data/global_metrics_local_*.csv
        collected_data/security_data_local_*.csv

══════════════════════════════════════════════════════════════════

【场景 2】本机 + 网络模拟采集 (Mac)

  终端 1 - 启动 NeuStack + 网络模拟:
    sudo bash scripts/mac/collect_local_tc.sh --rounds 3 --phase 60

  终端 2 - 生成流量:
    bash scripts/mac/traffic.sh 192.168.100.2 --duration 30 --mode heavy

  输出: collected_data/tcp_samples_local_tc_*.csv
        collected_data/global_metrics_local_tc_*.csv
        collected_data/security_data_local_tc_*.csv

══════════════════════════════════════════════════════════════════

【场景 3】服务器正常采集 (Linux)

  服务器 - 启动 NeuStack:
    sudo bash scripts/linux/collect.sh --hours 0

  客户端 - 生成流量 (Mac 或 Linux 均可):
    bash scripts/mac/traffic.sh YOUR_SERVER_IP --duration 15 --mode heavy
    # 或
    bash scripts/linux/traffic.sh YOUR_SERVER_IP --duration 15 --mode heavy

  输出: collected_data/tcp_samples_*.csv
        collected_data/global_metrics_*.csv
        collected_data/security_data_*.csv

══════════════════════════════════════════════════════════════════

【场景 4】服务器 + TC 采集 (Linux)

  服务器 - 启动 NeuStack + tc:
    sudo bash scripts/linux/collect_bwvar.sh --rounds 3 --phase-duration 60

  客户端 - 生成流量 (Mac 或 Linux 均可):
    bash scripts/mac/traffic.sh YOUR_SERVER_IP --duration 55 --mode heavy
    # 或
    bash scripts/linux/traffic.sh YOUR_SERVER_IP --duration 55 --mode heavy

  输出: collected_data/tcp_samples_bwvar_*.csv
        collected_data/global_metrics_bwvar_*.csv
        collected_data/security_data_bwvar_*.csv

══════════════════════════════════════════════════════════════════

【场景 5】Anomaly 短连接采集

  启动 NeuStack (本机或服务器均可，下面以本机为例):
    sudo bash scripts/mac/collect.sh --hours 0

  生成短连接流量 (Mac 或 Linux 均可):
    bash scripts/mac/traffic_anomaly.sh 192.168.100.2 --duration 15 --mode short
    # 或
    bash scripts/linux/traffic_anomaly.sh 10.0.1.2 --duration 15 --mode short

  输出: collected_data/tcp_samples_local_*.csv
        collected_data/global_metrics_local_*.csv (主要用于 Anomaly)
        collected_data/security_data_local_*.csv

══════════════════════════════════════════════════════════════════

【场景 6】Anomaly 混合连接采集

  启动 NeuStack:
    sudo bash scripts/mac/collect.sh --hours 0

  生成混合流量 (Mac 或 Linux 均可):
    bash scripts/mac/traffic_anomaly.sh 192.168.100.2 --duration 15 --mode mixed
    # 或
    bash scripts/linux/traffic_anomaly.sh 10.0.1.2 --duration 15 --mode mixed

  输出: collected_data/tcp_samples_local_*.csv
        collected_data/global_metrics_local_*.csv (主要用于 Anomaly)
        collected_data/security_data_local_*.csv

══════════════════════════════════════════════════════════════════

【场景 7】Security 正常流量采集 (可在本机或服务器)

  === 本机 (Mac) ===
  终端 1 - 启动 NeuStack + 防火墙:
    sudo bash scripts/mac/collect_security.sh --phase normal --duration 10

  终端 2 - 生成正常流量:
    bash scripts/mac/traffic_security.sh 192.168.100.2 --duration 10 --mode normal

  === 服务器 (Linux) ===
  服务器:
    sudo bash scripts/linux/collect_security.sh --phase normal --duration 10

  客户端 (Mac 或 Linux):
    bash scripts/mac/traffic_security.sh YOUR_SERVER_IP --duration 10 --mode normal
    # 或
    bash scripts/linux/traffic_security.sh YOUR_SERVER_IP --duration 10 --mode normal

  输出: collected_data/security_data_normal_*.csv (label=0)

══════════════════════════════════════════════════════════════════

【场景 8】Security 攻击流量采集

  === 本机 (Mac) ===
  终端 1:
    sudo bash scripts/mac/collect_security.sh --phase attack --duration 10

  终端 2:
    bash scripts/mac/traffic_security.sh 192.168.100.2 --duration 10 --mode attack

  === 服务器 (Linux) ===
  服务器:
    sudo bash scripts/linux/collect_security.sh --phase attack --duration 10

  客户端 (Mac 或 Linux):
    bash scripts/mac/traffic_security.sh YOUR_SERVER_IP --duration 10 --mode attack
    # 或
    bash scripts/linux/traffic_security.sh YOUR_SERVER_IP --duration 10 --mode attack

  输出: collected_data/security_data_attack_*.csv (label=1)

══════════════════════════════════════════════════════════════════

【数据合并与训练】

  合并所有 CSV 并生成训练数据集:
    python scripts/csv_to_dataset.py --data-dir collected_data/

  训练模型:
    cd training/orca && python train.py --data ../real_data/orca_dataset.npz
    cd training/bandwidth && python train.py --data ../real_data/bandwidth_dataset.npz
    cd training/anomaly && python train.py --data ../real_data/anomaly_dataset.npz
    cd training/security && python train.py --data ../real_data/security_dataset.npz

══════════════════════════════════════════════════════════════════

【注意事项】

  1. 每个场景的 CSV 会自动带上时间戳，不会覆盖
  2. csv_to_dataset.py 会自动合并目录下所有匹配的 CSV
  3. 建议每组至少采集 1000+ 样本 (约 10-15 分钟)
  4. Anomaly 需要看到多种正常模式，所以需要 5、6 两组额外数据
  5. Security 需要正常 (label=0) 和攻击 (label=1) 两组
  6. Security 建议在服务器上采集 (真实网络环境更有代表性)

EOF
