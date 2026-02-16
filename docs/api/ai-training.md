# AI Training Guide

This document describes how to train NeuStack's AI models.

## Table of Contents

- [Overview](#overview)
- [Environment Setup](#environment-setup)
- [Data Collection](#data-collection)
- [Model Training](#model-training)
- [ONNX Export](#onnx-export)
- [Deployment Verification](#deployment-verification)

---

## Overview

NeuStack includes four AI models:

| Model | Algorithm | Input Dimensions | Output | Purpose |
|-------|-----------|-----------------|--------|---------|
| **Orca** | SAC | 7-dim state | α ∈ [-1, 1] | Congestion window adjustment |
| **Bandwidth Prediction** | LSTM | 30×3 time series | bytes/s | Forward-looking estimation |
| **Anomaly Detection** | Autoencoder | 5-dim features | Reconstruction error | TCP anomaly detection |
| **Security Anomaly Detection** | Autoencoder | 8-dim features | Reconstruction error + confidence | Firewall threat detection |

### Orca Input Features (7-dim)

```
[cwnd_normalized,        # 当前拥塞窗口 / 最大窗口
 rtt_ratio,              # RTT / min_RTT
 loss_rate,              # 丢包率
 throughput_normalized,  # 吞吐量 / 带宽预测
 queue_delay,            # 排队延迟估计
 bandwidth_utilization,  # 带宽利用率
 trend]                  # 吞吐量变化趋势
```

### Bandwidth Prediction Input (30×3 time series)

```
[throughput,    # 瞬时吞吐量
 rtt,           # RTT
 loss_rate]     # 丢包率
```

### Anomaly Detection Input (5-dim)

```
[syn_rate,       # SYN 包速率
 rst_rate,       # RST 包速率
 new_conn_rate,  # 新连接速率
 packet_rate,    # 总包速率
 avg_pkt_size]   # 平均包大小
```

### Security Anomaly Detection Input (8-dim)

Dedicated to the firewall. Raw data is collected by `SecurityExporter` and normalized during training.

```
[pps_norm,             # 包速率
 bps_norm,             # 字节速率
 syn_rate_norm,        # SYN 速率（SYN Flood 指标）
 rst_rate_norm,        # RST 速率（端口扫描指标）
 syn_ratio_norm,       # SYN/SYN-ACK 比率
 new_conn_rate_norm,   # 新连接速率
 avg_pkt_size_norm,    # 平均包大小（小包攻击指标）
 rst_ratio_norm]       # RST/总包 比率
```

---

## Environment Setup

### Using Conda (Recommended)

```bash
conda env create -f training/environment.yml
conda activate neustack
```

### Using pip

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r training/requirements.txt
```

### Dependency List

```
torch>=2.0.0
numpy>=1.24.0
pandas>=2.0.0
matplotlib>=3.7.0
onnx>=1.14.0
onnxruntime>=1.15.0
pyyaml>=6.0
```

---

## Data Collection

### macOS

```bash
# 启动 NeuStack 并采集数据
sudo ./scripts/mac/collect.sh --duration 3600 --output collected_data/

# 或手动运行
sudo ./build/examples/neustack_demo --export metrics.csv
```

### Linux

```bash
sudo ./scripts/linux/collect.sh --duration 3600 --output collected_data/
```

### Data Format

```csv
timestamp_ms,syn_received,rst_received,conn_established,conn_reset,packets_rx,bytes_rx
1234567890,2,0,1,0,15,1024
1234567990,0,0,0,0,3,256
```

### Security Data Collection (SecurityExporter)

The firewall security model uses a dedicated `SecurityExporter` for data collection:

```cpp
#include "neustack/metrics/security_exporter.hpp"

SecurityExporter exporter("security_data.csv", firewall_ai.metrics());

// 定时器中每秒调用
exporter.flush(0);  // label=0 正常流量
exporter.flush(1);  // label=1 异常流量（手动标注/攻击注入时）
```

Output CSV format (15 columns):

```csv
timestamp_ms,packets_total,bytes_total,syn_packets,syn_ack_packets,rst_packets,pps,bps,syn_rate,rst_rate,syn_ratio,new_conn_rate,avg_pkt_size,rst_ratio,label
1707700000,5000,750000,100,95,10,500,75000,10,1,1.05,10,150,0.002,0
```

### Data Preprocessing

```bash
python scripts/csv_to_dataset.py collected_data/ training/real_data/
```

---

## Model Training

### Orca (SAC Congestion Control)

```bash
cd training/orca

# 编辑配置
vim config.yaml

# 训练
python train.py --config config.yaml

# 可视化训练曲线
python plot_training.py checkpoints/
```

#### config.yaml

```yaml
model:
  actor_hidden: [256, 256]
  critic_hidden: [256, 256]
  
training:
  episodes: 10000
  batch_size: 256
  learning_rate: 0.0003
  gamma: 0.99
  tau: 0.005
  
environment:
  bandwidth_mbps: 100
  delay_ms: 20
  loss_rate: 0.01
```

### Bandwidth Prediction (LSTM)

```bash
cd training/bandwidth

python train.py --config config.yaml
```

#### config.yaml

```yaml
model:
  input_dim: 3
  hidden_dim: 64
  num_layers: 2
  seq_len: 30
  
training:
  epochs: 100
  batch_size: 64
  learning_rate: 0.001
```

### Anomaly Detection (Autoencoder)

```bash
cd training/anomaly

python train.py --config config.yaml
```

#### config.yaml

```yaml
model:
  input_dim: 5
  hidden_dims: [32, 16]
  latent_dim: 8
  
training:
  epochs: 100
  batch_size: 64
  learning_rate: 0.001
```

### Security Anomaly Detection (Autoencoder)

```bash
cd training/security

python train.py --config config.yaml --data ../../security_data.csv
```

#### config.yaml

```yaml
model:
  input_dim: 8
  hidden_dims: [32, 16]
  latent_dim: 4

training:
  epochs: 200
  batch_size: 64
  learning_rate: 0.001
  threshold_percentile: 99  # 正常数据的 99 分位数作为阈值
```

> **Note**: Training data is collected by `SecurityExporter`. Raw values in the CSV need to be normalized to [0, 1] on the training side.

---

## ONNX Export

After training is complete, export to ONNX format:

```bash
# Orca
cd training/orca
python export_onnx.py --checkpoint checkpoints/best_model.pth

# 带宽预测
cd training/bandwidth
python export_onnx.py --checkpoint checkpoints/best_model.pth

# 异常检测
cd training/anomaly
python export_onnx.py --checkpoint checkpoints/best_model.pth

# 安全异常检测
cd training/security
python export_onnx.py --checkpoint checkpoints/best_model.pth
```

Exported files:
- `models/orca_actor.onnx`
- `models/bandwidth_predictor.onnx`
- `models/anomaly_detector.onnx`
- `models/security_anomaly.onnx`

### Verify ONNX

```bash
python -c "import onnx; onnx.checker.check_model(onnx.load('models/orca_actor.onnx'))"
```

---

## Deployment Verification

### Build with AI Enabled

```bash
cmake -B build -DNEUSTACK_ENABLE_AI=ON
cmake --build build
```

### Run Tests

```bash
./build/examples/ai_test
```

Expected output:
```
=== AI Actions ===
  [CWND] alpha=0.187024
  [BW_PRED] predicted=47882284 bytes/s
  [ANOMALY] score=0.0123
```

### NetworkAgent State Monitoring

```cpp
auto& agent = stack->network_agent();

// 获取当前状态
auto state = agent.current_state();
printf("Agent state: %s\n", agent.state_name(state));

// 获取统计
auto stats = agent.stats();
printf("State transitions: %lu\n", stats.state_transitions);
```

---

## Troubleshooting

### Training Does Not Converge

1. Check if learning rate is too high
2. Increase training data volume
3. Adjust network architecture

### ONNX Export Fails

1. Ensure PyTorch version >= 2.0
2. Check if the model has dynamic shapes
3. Use `opset_version=17`

### Abnormal C++ Inference Results

1. Check input data normalization
2. Confirm ONNX Runtime version compatibility
3. Verify model input/output dimensions

---

## References

- [SAC Paper](https://arxiv.org/abs/1801.01290)
- [Orca: A Congestion Control System](https://dl.acm.org/doi/10.1145/3544216.3544242)
- [PyTorch ONNX Export](https://pytorch.org/docs/stable/onnx.html)
