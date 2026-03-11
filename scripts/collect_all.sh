#!/bin/bash
# scripts/collect_all.sh
#
# NeuStack 完整数据采集指南 (1 Mac + 1 Linux 服务器)
#
# 采集 6 步，训练 4 个模型:
#
# ┌──────┬──────────────────────┬────────────┬────────────────────────┐
# │ 步骤 │ 采集内容             │ 训练模型   │ 采集环境               │
# ├──────┼──────────────────────┼────────────┼────────────────────────┤
# │  1   │ Mac 本地正常流量     │ Orca/BW    │ Mac 两个终端           │
# │  2   │ Mac 本地 + 网络模拟  │ Orca/BW    │ Mac 两个终端           │
# │  3   │ 服务器正常流量       │ Orca/BW    │ Linux + Mac            │
# │  4   │ 服务器 + 带宽变化    │ Orca/BW    │ Linux + Mac            │
# │  5   │ Anomaly 流量模式     │ Anomaly    │ Mac 两个终端 (2 轮)    │
# │  6   │ Security 正常+攻击   │ Security   │ Mac 两个终端 (2 轮)    │
# └──────┴──────────────────────┴────────────┴────────────────────────┘
#
# 产出文件 (collected_data/ 目录下):
#   Orca/BW:   tcp_samples_{local_normal,local_tc,server_normal,server_tc}_*.csv
#   Anomaly:   global_metrics_{local_normal,local_tc,...,local_anomaly_short,local_anomaly_mix}_*.csv
#   Security:  security_data_{normal,attack}_*.csv
#
# 前置条件:
#   1. Mac 已编译: cmake -B build && cmake --build build
#   2. Linux 已编译 (同上)
#   3. Linux 服务器 Mac 可 SSH 访问
#   4. Linux 安装了 socat: sudo apt install socat

cat << 'GUIDE'
╔══════════════════════════════════════════════════════════════════╗
║         NeuStack 完整数据采集 (1 Mac + 1 Linux)                  ║
╚══════════════════════════════════════════════════════════════════╝

前置准备:
  Mac:    cd /path/to/NeuStack && cmake -B build && cmake --build build
  Linux:  同上, 另外 sudo apt install socat

将 YOUR_SERVER_IP 替换为你的 Linux 服务器公网/内网 IP。

══════════════════════════════════════════════════════════════════
【步骤 1/6】Mac 本地正常流量                            ~15 分钟
             → tcp_samples_local_normal_*.csv (Orca/BW 训练)
══════════════════════════════════════════════════════════════════

  Mac 终端 1:
    sudo bash scripts/mac/collect.sh --hours 0

  Mac 终端 2:
    bash scripts/mac/traffic.sh 192.168.100.2 --duration 15 --mode heavy

  流量跑完后在终端 1 按 Ctrl+C 停止。

══════════════════════════════════════════════════════════════════
【步骤 2/6】Mac 本地 + 网络模拟 (延迟/丢包/限速)       ~30 分钟
             → tcp_samples_local_tc_*.csv (Orca/BW 训练)
══════════════════════════════════════════════════════════════════

  Mac 终端 1:
    sudo bash scripts/mac/collect_local_tc.sh --rounds 3 --phase 60

  Mac 终端 2 (等终端 1 显示 "Ready!" 后):
    bash scripts/mac/traffic.sh 192.168.100.2 --duration 30 --mode heavy

  自动结束 (3 轮 × 8 阶段 × 60s = 24 分钟)。

══════════════════════════════════════════════════════════════════
【步骤 3/6】Linux 服务器正常流量                        ~15 分钟
             → tcp_samples_server_normal_*.csv (Orca/BW 训练)
══════════════════════════════════════════════════════════════════

  Linux 服务器:
    sudo bash scripts/linux/collect.sh --hours 0

  Mac (等服务器显示 "Ready!" 后):
    bash scripts/mac/traffic.sh YOUR_SERVER_IP --http-port 8080 --duration 15 --mode heavy

  流量跑完后在 Linux 按 Ctrl+C 停止。

══════════════════════════════════════════════════════════════════
【步骤 4/6】Linux 服务器 + 带宽变化 (tc)               ~30 分钟
             → tcp_samples_server_tc_*.csv (Orca/BW 训练)
══════════════════════════════════════════════════════════════════

  Linux 服务器:
    sudo bash scripts/linux/collect_bwvar.sh --rounds 3 --phase-duration 60

  Mac (等服务器显示 "Ready!" 后):
    bash scripts/mac/traffic.sh YOUR_SERVER_IP --http-port 8080 --duration 35 --mode heavy

  自动结束 (3 轮 × 10 阶段 × 60s = 30 分钟)。

══════════════════════════════════════════════════════════════════
【步骤 5/6】Anomaly 流量模式 (两轮)                     ~30 分钟
             → global_metrics_local_anomaly_{short,mix}_*.csv
══════════════════════════════════════════════════════════════════

  --- 5a: 短连接模式 (~15 分钟) ---

  Mac 终端 1:
    sudo bash scripts/mac/collect.sh --hours 0 --suffix local_anomaly_short

  Mac 终端 2:
    bash scripts/mac/traffic_anomaly.sh 192.168.100.2 --duration 15 --mode short

  流量跑完后 Ctrl+C。

  --- 5b: 混合连接模式 (~15 分钟) ---

  Mac 终端 1:
    sudo bash scripts/mac/collect.sh --hours 0 --suffix local_anomaly_mix

  Mac 终端 2:
    bash scripts/mac/traffic_anomaly.sh 192.168.100.2 --duration 15 --mode mixed

  流量跑完后 Ctrl+C。

══════════════════════════════════════════════════════════════════
【步骤 6/6】Security 正常 + 攻击 (两轮)                ~30 分钟
             → security_data_{normal,attack}_*.csv
══════════════════════════════════════════════════════════════════

  --- 6a: 正常流量 (label=0, ~15 分钟) ---

  Mac 终端 1:
    sudo bash scripts/mac/collect_security.sh --phase normal --duration 15

  Mac 终端 2:
    bash scripts/mac/traffic_security.sh 192.168.100.2 --duration 15 --mode normal

  等待 15 分钟自动结束。

  --- 6b: 攻击流量 (label=1, ~15 分钟) ---

  Mac 终端 1:
    sudo bash scripts/mac/collect_security.sh --phase attack --duration 15

  Mac 终端 2:
    bash scripts/mac/traffic_security.sh 192.168.100.2 --duration 15 --mode attack

  等待 15 分钟自动结束。

══════════════════════════════════════════════════════════════════
【数据合并 & 训练】
══════════════════════════════════════════════════════════════════

  # 检查采集结果
  ls -la collected_data/*.csv

  # 生成训练数据集 (一条命令全部生成)
  python3 scripts/python/csv_to_dataset.py --data-dir collected_data/

  # 训练各模型
  cd training/orca     && python train.py --data ../real_data/orca_dataset.npz && cd ../..
  cd training/bandwidth && python train.py --data ../real_data/bandwidth_dataset.npz && cd ../..
  cd training/anomaly  && python train.py --data ../real_data/anomaly_dataset.npz && cd ../..
  cd training/security && python train.py --data ../real_data/security_dataset.npz && cd ../..

  # 导出 ONNX
  cd training/security && python export_onnx.py && cd ../..

══════════════════════════════════════════════════════════════════
【注意事项】
══════════════════════════════════════════════════════════════════

  1. 每步的 CSV 带时间戳，不会覆盖，可安全重复运行
  2. csv_to_dataset.py 自动合并 collected_data/ 下所有匹配的 CSV
  3. 步骤 1-4 的 security_data 也会被采集（顺带），会作为 label=0 正常数据
  4. 步骤 5 只用 global_metrics，tcp_samples 是额外收获
  5. 步骤 6 的攻击强度已大幅提升 (200-500 并发)
  6. 总采集时间约 2.5 小时
  7. 如果 Linux 服务器不可用，可以跳过步骤 3-4，
     本地数据也够训练，只是缺少真实网络环境的多样性

GUIDE
