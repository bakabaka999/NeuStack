# AI Training Guide

本文档介绍如何训练 NeuStack 的 AI 模型。

## 目录

- [概述](#概述)
- [环境配置](#环境配置)
- [数据采集](#数据采集)
- [模型训练](#模型训练)
- [ONNX 导出](#onnx-导出)
- [部署验证](#部署验证)

---

## 概述

NeuStack 包含三个 AI 模型：

| 模型 | 算法 | 输入维度 | 输出 | 用途 |
|------|------|----------|------|------|
| **Orca** | SAC | 7 维状态 | α ∈ [-1, 1] | 拥塞窗口调节 |
| **带宽预测** | LSTM | 30×3 时序 | bytes/s | 前瞻性估计 |
| **异常检测** | Autoencoder | 5 维特征 | 重构误差 | 攻击检测 |

### Orca 输入特征 (7 维)

```
[cwnd_normalized,        # 当前拥塞窗口 / 最大窗口
 rtt_ratio,              # RTT / min_RTT
 loss_rate,              # 丢包率
 throughput_normalized,  # 吞吐量 / 带宽预测
 queue_delay,            # 排队延迟估计
 bandwidth_utilization,  # 带宽利用率
 trend]                  # 吞吐量变化趋势
```

### 带宽预测输入 (30×3 时序)

```
[throughput,    # 瞬时吞吐量
 rtt,           # RTT
 loss_rate]     # 丢包率
```

### 异常检测输入 (5 维)

```
[syn_rate,       # SYN 包速率
 rst_rate,       # RST 包速率
 new_conn_rate,  # 新连接速率
 packet_rate,    # 总包速率
 avg_pkt_size]   # 平均包大小
```

---

## 环境配置

### 使用 Conda (推荐)

```bash
conda env create -f training/environment.yml
conda activate neustack
```

### 使用 pip

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r training/requirements.txt
```

### 依赖清单

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

## 数据采集

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

### 数据格式

```csv
timestamp_ms,syn_received,rst_received,conn_established,conn_reset,packets_rx,bytes_rx
1234567890,2,0,1,0,15,1024
1234567990,0,0,0,0,3,256
```

### 数据预处理

```bash
python scripts/csv_to_dataset.py collected_data/ training/real_data/
```

---

## 模型训练

### Orca (SAC 拥塞控制)

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

### 带宽预测 (LSTM)

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

### 异常检测 (Autoencoder)

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

---

## ONNX 导出

训练完成后，导出为 ONNX 格式：

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
```

导出文件：
- `models/orca_actor.onnx`
- `models/bandwidth_predictor.onnx`
- `models/anomaly_detector.onnx`

### 验证 ONNX

```bash
python -c "import onnx; onnx.checker.check_model(onnx.load('models/orca_actor.onnx'))"
```

---

## 部署验证

### 编译启用 AI

```bash
cmake -B build -DNEUSTACK_ENABLE_AI=ON
cmake --build build
```

### 运行测试

```bash
./build/examples/ai_test
```

预期输出：
```
=== AI Actions ===
  [CWND] alpha=0.187024
  [BW_PRED] predicted=47882284 bytes/s
  [ANOMALY] score=0.0123
```

### NetworkAgent 状态监控

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

## 故障排查

### 训练不收敛

1. 检查学习率是否过大
2. 增加训练数据量
3. 调整网络架构

### ONNX 导出失败

1. 确保 PyTorch 版本 >= 2.0
2. 检查模型是否有动态形状
3. 使用 `opset_version=17`

### C++ 推理结果异常

1. 检查输入数据归一化
2. 确认 ONNX Runtime 版本兼容
3. 验证模型输入输出维度

---

## 参考资料

- [SAC Paper](https://arxiv.org/abs/1801.01290)
- [Orca: A Congestion Control System](https://dl.acm.org/doi/10.1145/3544216.3544242)
- [PyTorch ONNX Export](https://pytorch.org/docs/stable/onnx.html)
