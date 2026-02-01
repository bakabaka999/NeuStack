# Phase 2: AI 增强网络协议栈

## 1. 概述

Phase 2 为 NeuStack 引入 AI/ML 能力，实现三大核心功能：

| 功能 | 目标 | 方法 | 难度 |
|------|------|------|:----:|
| **异常检测** | 识别攻击流量与异常行为 | LSTM-Autoencoder | ⭐⭐⭐ |
| **AI 拥塞控制** | 自适应调整 cwnd | Orca (DDPG) | ⭐⭐⭐⭐ |
| **带宽预测** | 预测可用带宽 | LSTM | ⭐⭐⭐ |

```
┌─────────────────────────────────────────────────────────────────┐
│                      NeuStack AI Architecture                    │
│                                                                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐              │
│  │  Congestion │  │   Anomaly   │  │  Bandwidth  │              │
│  │   Control   │  │  Detection  │  │  Prediction │              │
│  │   (Orca)    │  │ (LSTM-AE)   │  │   (LSTM)    │              │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘              │
│         │                │                │                      │
│         └────────────────┼────────────────┘                      │
│                          │                                       │
│                ┌─────────▼─────────┐                            │
│                │  Metrics Framework │                            │
│                └─────────┬─────────┘                            │
│                          │                                       │
│         ┌────────────────┼────────────────┐                     │
│         ▼                ▼                ▼                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │     TCP     │  │     IP      │  │     HAL     │             │
│  └─────────────┘  └─────────────┘  └─────────────┘             │
└─────────────────────────────────────────────────────────────────┘
```

### 设计考量

**为什么不用 RouteNet (GNN)?**

RouteNet 需要**整个网络的拓扑信息**（路由器、链路、流量矩阵），适用于 SDN 控制器或 ISP 核心网。而 NeuStack 是**端主机协议栈**，只能观测到自己的连接状态（RTT、ACK 速率等），看不到网络内部结构。

因此我们用 **LSTM 时序预测**替代 GNN，基于历史吞吐量预测未来带宽——这是端主机能做的事情。

---

## 2. 参考论文

### 2.1 拥塞控制: Orca

**论文**: [Orca: Pragmatic Learning-based Congestion Control](https://www.usenix.org/conference/nsdi22/presentation/abbasloo)
**发表**: NSDI 2022
**作者**: Soheil Abbasloo 等

**核心思想**:
- 使用 **DDPG (Deep Deterministic Policy Gradient)** 强化学习
- 双层控制: TCP Cubic 做基础，RL 做调制
- 输出动作: `cwnd = 2^α × cwnd_tcp` (α ∈ [-1, 1])

**架构**:
```
输入 (状态):                    输出 (动作):
├── throughput                  α ∈ [-1, 1]
├── queuing_delay               cwnd = 2^α × cwnd_cubic
├── rtt / min_rtt
└── loss_rate
         │
         ▼
┌─────────────────┐
│  DDPG Network   │
│  (Actor-Critic) │
└─────────────────┘
```

**为什么选 Orca 而非 Canopy?**

Canopy 在 Orca 基础上增加了"抽象解释器"和"量化证书"来保证最坏情况行为，但这需要复杂的验证工具链。对于保研面试项目，**Orca 已经足够展示 RL + 网络的创新点**，Canopy 可以作为"未来工作"提及。

---

### 2.2 异常检测: LSTM-Autoencoder

**核心论文**:

| 论文 | 链接 | 亮点 |
|------|------|------|
| Multi-Scale CNN-RNN Autoencoder | [arXiv:2204.03779](https://arxiv.org/abs/2204.03779) | 无监督, LSTM+Isolation Forest |
| Enhanced IIoT IDS | [arXiv:2501.15266](https://arxiv.org/abs/2501.15266) | 99.94% F1, 边缘部署 0.185ms |

**为什么异常检测最容易落地?**

1. **旁路运行**: 不在数据面关键路径上，推理慢一点不影响网速
2. **容错性高**: 即使误报也只是打印警告，不会破坏连接
3. **无监督学习**: 只用正常流量训练，不需要标注攻击数据

**架构**:
```
正常流量 ──▶ ┌─────────┐    ┌─────────┐
             │ Encoder │───▶│ Decoder │──▶ 重构流量
             └─────────┘    └─────────┘
                                │
                                ▼
                         重构误差 > 阈值?
                                │
                        ┌───────┴───────┐
                        ▼               ▼
                      正常            异常!
```

---

### 2.3 带宽预测: LSTM

**参考工作**:

| 论文 | 链接 | 亮点 |
|------|------|------|
| Bi-LSTM Bandwidth Prediction | [ETRI 2024](https://onlinelibrary.wiley.com/doi/full/10.4218/etrij.2022-0459) | 3G/4G/5G 带宽预测 |
| Microsoft BW Challenge | [MMSys 2024](https://arxiv.org/html/2403.06324v1) | 实时通信, <10MB 模型 |

**应用场景**:
- 指导视频流码率选择 (应用层)
- 辅助设置 `ssthresh`
- 提前预警带宽下降

---

## 3. 统一数据采集框架

### 3.1 架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     Metrics Framework                            │
│                                                                  │
│  数据面 (Data Plane) - 无锁、高频                                 │
│  ┌───────────────────────────────────────────────────────┐      │
│  │  TCP ACK Handler                                       │      │
│  │       │                                                │      │
│  │       ▼ (无锁写入)                                     │      │
│  │  ┌─────────────────────────────────────────────────┐  │      │
│  │  │            Lock-Free Ring Buffer                 │  │      │
│  │  │  [sample][sample][sample][sample][sample]...    │  │      │
│  │  └─────────────────────────────────────────────────┘  │      │
│  └───────────────────────────────────────────────────────┘      │
│                            │                                     │
│                            │ (定期快照, 10-100ms)                │
│                            ▼                                     │
│  智能面 (Intelligence Plane) - 异步、低频                        │
│  ┌───────────────────────────────────────────────────────┐      │
│  │  ┌───────────┐      ┌───────────┐      ┌───────────┐  │      │
│  │  │  Anomaly  │      │ Congestion│      │ Bandwidth │  │      │
│  │  │ Detection │      │  Control  │      │ Prediction│  │      │
│  │  └───────────┘      └───────────┘      └───────────┘  │      │
│  └───────────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────────┘
```

**关键设计**: 数据面和智能面解耦
- TCP 线程只往 Ring Buffer 无锁写入，不做任何计算
- AI 线程定期读取快照，计算特征，推理模型
- 避免 AI 推理阻塞网络处理

### 3.2 采集层次

| 层次 | 频率 | 数据 | 处理方式 |
|------|------|------|----------|
| L1: Per-Packet | 每包 | 时间戳、大小 | 写入 Ring Buffer |
| L2: Per-ACK | 每 ACK | RTT、cwnd | 写入 Ring Buffer |
| L3: Snapshot | 10-100ms | 聚合统计 | AI 推理输入 |
| L4: Event | 事件触发 | 连接状态变化 | 日志 + 异常检测 |

---

## 4. 数据需求

### 4.1 拥塞控制 (Orca)

**输入特征** (每 10-50ms 采样一次):

| 指标 | 类型 | 说明 | 归一化 |
|------|------|------|--------|
| `throughput` | gauge | 采样周期吞吐量 | / 估计带宽 |
| `queuing_delay` | gauge | RTT - min_RTT | / min_RTT |
| `rtt_ratio` | gauge | RTT / min_RTT | 已归一化 |
| `loss_rate` | gauge | 丢包率 | 原值 [0,1] |

**输出动作**:
```cpp
float alpha;  // cwnd_new = 2^alpha * cwnd_cubic, alpha ∈ [-1, 1]
```

**推理频率**: 每 RTT 或每 10ms，**不是每个 ACK**

---

### 4.2 异常检测 (LSTM-Autoencoder)

**输入特征** (每 1 秒聚合):

| 指标 | 说明 | 检测目标 |
|------|------|----------|
| `syn_rate` | SYN 包速率 | SYN Flood |
| `rst_rate` | RST 包速率 | 端口扫描 |
| `new_conn_rate` | 新建连接速率 | DDoS |
| `packet_rate` | 总包速率 | 流量异常 |
| `avg_packet_size` | 平均包大小 | 协议异常 |

**推理频率**: 每 1 秒

---

### 4.3 带宽预测 (LSTM)

**输入特征** (时序窗口):

| 指标 | 说明 |
|------|------|
| `throughput_history[N]` | 过去 N 个采样周期的吞吐量 |
| `rtt_history[N]` | 过去 N 个采样周期的 RTT |
| `loss_history[N]` | 过去 N 个采样周期的丢包率 |

**输出**: 未来 1 秒的预测带宽

**推理频率**: 每 100ms - 1s

---

### 4.4 指标汇总表

| 指标名 | 类型 | 采集点 | 拥塞控制 | 异常检测 | 带宽预测 |
|--------|------|--------|:--------:|:--------:|:--------:|
| `rtt_us` | gauge | Per-ACK | ✓ | | ✓ |
| `min_rtt_us` | gauge | Per-ACK | ✓ | | |
| `cwnd` | gauge | Per-ACK | ✓ | | |
| `throughput` | gauge | Periodic | ✓ | ✓ | ✓ |
| `packets_sent` | counter | Per-Send | ✓ | ✓ | |
| `packets_retrans` | counter | Event | ✓ | | ✓ |
| `syn_received` | counter | Event | | ✓ | |
| `rst_received` | counter | Event | | ✓ | |

---

## 5. 推理延迟考量

### 问题

TCP 处理在微秒级，但 ONNX Runtime 推理可能需要 0.1-1ms。如果每个 ACK 都跑神经网络，CPU 会爆。

### 解决方案

| 策略 | 说明 |
|------|------|
| **异步推理** | AI 线程独立运行，不阻塞数据面 |
| **降低频率** | 每 10ms 或每 RTT 推理一次，不是每 ACK |
| **模型轻量化** | 使用小型 MLP (< 1MB)，或量化模型 |
| **缓存决策** | 推理结果缓存，多个 ACK 复用同一个 α |

```
┌─────────────────────────────────────────────────────────────────┐
│  时间线                                                          │
│                                                                  │
│  ACK ACK ACK ACK ACK ACK ACK ACK ACK ACK ACK ACK ACK ACK        │
│   │   │   │   │   │   │   │   │   │   │   │   │   │   │        │
│   └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘        │
│                           │                                      │
│                           ▼                                      │
│                    ┌─────────────┐                              │
│                    │  Snapshot   │  每 10ms                     │
│                    │  + 推理     │                              │
│                    └──────┬──────┘                              │
│                           │                                      │
│                           ▼                                      │
│                      更新 α 值                                   │
│                    (后续 ACK 复用)                               │
└─────────────────────────────────────────────────────────────────┘
```

---

## 6. 教程规划

| 编号 | 主题 | 内容 | 难度 | 优先级 |
|------|------|------|:----:|:------:|
| 01 | 双线程架构 | 数据面/智能面分离, SPSCQueue | ⭐⭐ | **P0** |
| 02 | 指标采集 | TCPSample, GlobalMetrics, 采集点 | ⭐⭐ | **P0** |
| 03 | ONNX 集成 | ONNX Runtime + 推理接口 | ⭐⭐ | **P0** |
| 04 | 异常检测 | LSTM-AE 模型集成 | ⭐⭐⭐ | **P1** |
| 05 | 拥塞控制 AI | Orca DDPG 实现 | ⭐⭐⭐⭐ | P2 |
| 06 | 带宽预测 | LSTM 时序预测 | ⭐⭐⭐ | P2 |

**建议路径**: 01 → 02 → 03 → 04 (异常检测) → 展示 + 准备问答

---

## 7. 实现路线图

### Phase 2.1: 基础设施 (必做)
- [x] 实现无锁 Ring Buffer (MetricsBuffer, StreamBuffer)
- [ ] 实现 SPSCQueue (智能面 → 数据面)
- [ ] 定义 TCPSample / GlobalMetrics
- [ ] 实现智能面线程骨架
- [ ] 在 TCP 关键路径埋入采集点
- [ ] 验证性能影响 (Benchmark)

### Phase 2.2: 异常检测 (推荐)
- [ ] 集成 ONNX Runtime
- [ ] 实现特征提取
- [ ] 加载预训练 LSTM-AE 模型
- [ ] 实时异常检测 Demo

### Phase 2.3: 拥塞控制 (进阶)
- [ ] 实现 Orca 风格控制器骨架
- [ ] 集成 ONNX 模型
- [ ] 对比 Cubic vs Orca 性能

### Phase 2.4: 带宽预测 (可选)
- [ ] LSTM 模型集成
- [ ] 应用层 API 暴露

---

## 8. 面试亮点包装

### 可以讲的点

1. **架构设计**: 数据面/智能面分离，无锁 Ring Buffer
2. **论文理解**: Orca 的双层控制思想，LSTM-AE 的异常检测原理
3. **工程权衡**: 为什么用 LSTM 而非 GNN，推理频率的选择
4. **性能意识**: 如何避免 AI 推理阻塞网络处理

### 可能被问的问题

| 问题 | 回答思路 |
|------|----------|
| 为什么不用 RouteNet? | 端主机看不到全网拓扑，只能用时序预测 |
| AI 推理会不会太慢? | 异步推理 + 降低频率 + 模型轻量化 |
| 怎么训练模型? | 先用开源数据集/模拟器，重点展示推理集成 |
| Orca 和 BBR 有什么区别? | BBR 是启发式，Orca 是 RL；目标类似但方法不同 |

---

## 9. 参考资料

### 论文
- [Orca: Pragmatic Learning-based Congestion Control](https://www.usenix.org/conference/nsdi22/presentation/abbasloo) - NSDI 2022
- [Canopy: Property-Driven Learning](https://arxiv.org/abs/2412.10915) - EuroSys 2026 (进阶)
- [LSTM-Autoencoder for Intrusion Detection](https://arxiv.org/abs/2204.03779)
- [Bi-LSTM Bandwidth Prediction](https://onlinelibrary.wiley.com/doi/full/10.4218/etrij.2022-0459)

### 工具
- [ONNX Runtime](https://onnxruntime.ai/) - 跨平台推理引擎
- [PyTorch](https://pytorch.org/) - 训练框架

### 数据集
- [CIC-IDS](https://www.unb.ca/cic/datasets/) - 入侵检测数据集
- [Pantheon](https://github.com/StanfordSNR/pantheon) - 拥塞控制测试平台
