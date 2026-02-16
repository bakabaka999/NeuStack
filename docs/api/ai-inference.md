# AI Inference API Reference

NeuStack's AI subsystem is powered by the **Intelligence Plane**, which coordinates inference results from four models through the **NetworkAgent** decision layer.

## Architecture Overview

```
Data Plane (TCP/Firewall)                Intelligence Plane (AI Thread)
┌──────────────┐    MetricsBuffer     ┌──────────────────────┐
│  TCPLayer    │ ──── Metric Sampling ────→  │  IntelligencePlane   │
│  Firewall    │                      │  ├─ OrcaModel        │
│              │    SPSCQueue         │  ├─ BandwidthPredict  │
│              │ ←── AIAction ──────  │  ├─ AnomalyDetector  │
└──────────────┘                      │  └─ SecurityAnomaly  │
                                      └──────────┬───────────┘
                                                  │
                                        ┌─────────▼─────────┐
                                        │   NetworkAgent     │
                                        │   (4-State Decision Layer)    │
                                        └───────────────────┘
```

The data plane and intelligence plane communicate through lock-free queues with zero-copy and zero-allocation.

---

## IntelligencePlane — Intelligence Plane

```cpp
#include "neustack/ai/intelligence_plane.hpp"
```

The intelligence plane runs on a dedicated thread, executing model inference at configured intervals.

### Configuration

```cpp
IntelligencePlaneConfig config;

// 模型路径（空字符串 = 禁用该模型）
config.orca_model_path      = "models/orca_actor.onnx";
config.anomaly_model_path   = "models/anomaly_detector.onnx";
config.bandwidth_model_path = "models/bandwidth_predictor.onnx";

// 推理间隔
config.orca_interval      = std::chrono::milliseconds(10);    // 10ms
config.anomaly_interval   = std::chrono::milliseconds(1000);  // 1s
config.bandwidth_interval = std::chrono::milliseconds(100);   // 100ms

// 异常检测阈值
config.anomaly_threshold = 0.5f;

// 带宽预测历史长度
config.bandwidth_history_length = 30;
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `orca_model_path` | `string` | `""` | Orca SAC Actor model path |
| `anomaly_model_path` | `string` | `""` | Traffic anomaly detection model path |
| `bandwidth_model_path` | `string` | `""` | LSTM bandwidth prediction model path |
| `orca_interval` | `ms` | 10 | Orca inference interval |
| `anomaly_interval` | `ms` | 1000 | Anomaly detection interval |
| `bandwidth_interval` | `ms` | 100 | Bandwidth prediction interval |
| `anomaly_threshold` | `float` | 0.5 | Anomaly detection reconstruction error threshold |
| `bandwidth_history_length` | `size_t` | 30 | History length required for bandwidth prediction |

### Usage

```cpp
// 数据通道（数据面分配）
MetricsBuffer<TCPSample, 1024> metrics_buf;
SPSCQueue<AIAction, 16> action_queue;

// 创建并启动
IntelligencePlane plane(metrics_buf, action_queue, config);
plane.start();

// ... 运行中 ...

plane.stop();
```

### Threading Model

- The intelligence plane runs on a separate thread without blocking the data plane
- Reads data plane metrics via `MetricsBuffer` (lock-free ring buffer)
- Sends `AIAction` back to the data plane via `SPSCQueue` (single-producer single-consumer)
- All communication is lock-free and allocation-free

---

## ONNXInference — ONNX Inference Engine

```cpp
#include "neustack/ai/onnx_inference.hpp"
```

The shared ONNX Runtime wrapper used by all AI models.

### Construction

```cpp
// 加载模型
ONNXInference model("model.onnx");           // 默认 1 线程
ONNXInference model("model.onnx", 4);        // 4 线程推理
```

### Query Interface

```cpp
model.input_name();       // 输入张量名称
model.output_name();      // 输出张量名称
model.input_shape();      // 输入形状 vector<int64_t>
model.output_shape();     // 输出形状 vector<int64_t>
model.input_size();       // 输入元素数
model.output_size();      // 输出元素数

// 模型 metadata（训练时写入的自定义字段）
auto val = model.get_metadata("threshold");  // std::optional<string>
```

### Inference

```cpp
// 单样本推理
std::vector<float> input = {1.0f, 2.0f, 3.0f};
auto output = model.run(input);

// 批量推理
auto batch_output = model.run_batch(batch_data, batch_size);

// 原地推理（避免分配）
std::vector<float> out_buf(model.output_size());
model.run_inplace(input, out_buf);
```

> **Thread Safety**: The `run()` family of methods can be called concurrently.

---

## Model API

All models inherit from the `AIModel` base class:

```cpp
class AIModel {
public:
    virtual bool is_loaded() const = 0;   // 模型是否加载成功
    virtual const char* name() const = 0; // 模型名称
};
```

### OrcaModel — SAC Congestion Control

```cpp
#include "neustack/ai/orca_model.hpp"
```

Reinforcement learning congestion control based on Orca (NSDI 2022). Outputs a cwnd adjustment factor.

```cpp
OrcaModel orca("models/orca_actor.onnx");

ICongestionModel::Input input {
    .throughput_normalized    = 0.5f,  // 吞吐量（归一化）
    .queuing_delay_normalized = 0.1f,  // 排队延迟（归一化）
    .rtt_ratio                = 1.2f,  // RTT / min_RTT
    .loss_rate                = 0.01f, // 丢包率
    .cwnd_normalized          = 0.3f,  // cwnd（归一化）
    .in_flight_ratio          = 0.8f,  // in_flight / cwnd
    .predicted_bw_normalized  = 0.6f,  // 带宽预测结果（归一化）
};

auto result = orca.infer(input);
if (result) {
    float alpha = result->alpha;  // ∈ [-1, 1]
    // cwnd_new = 2^alpha * cwnd_base
}
```

| Input Features (7-dim) | Description |
|------------------------|-------------|
| `throughput_normalized` | Current throughput / max bandwidth |
| `queuing_delay_normalized` | Queuing delay / baseline RTT |
| `rtt_ratio` | Current RTT / min RTT |
| `loss_rate` | Packet loss rate [0, 1] |
| `cwnd_normalized` | Current cwnd / max cwnd |
| `in_flight_ratio` | In-flight data / cwnd |
| `predicted_bw_normalized` | BandwidthPredictor output (normalized) |

| Output | Description |
|--------|-------------|
| `alpha` | cwnd adjustment factor ∈ [-1, 1], `cwnd_new = 2^alpha * cwnd_base` |

### BandwidthPredictor — LSTM Bandwidth Prediction

```cpp
#include "neustack/ai/bandwidth_model.hpp"
```

LSTM-based time-series bandwidth prediction.

```cpp
BandwidthPredictor bw_pred(
    "models/bandwidth_predictor.onnx",
    30,                    // 历史长度（时间步数）
    10 * 1000 * 1000       // max_bandwidth = 10 MB/s（归一化基准）
);

IBandwidthModel::Input input {
    .throughput_history = { /* 30 个历史吞吐量 */ },
    .rtt_history        = { /* 30 个历史 RTT */ },
    .loss_history       = { /* 30 个历史丢包率 */ },
};

auto result = bw_pred.infer(input);
if (result) {
    uint32_t predicted_bw = result->predicted_bandwidth;  // bytes/s
    float confidence      = result->confidence;           // [0, 1]
}

// 查询所需历史长度
size_t len = bw_pred.required_history_length();  // 30
```

| Input | Description |
|-------|-------------|
| `throughput_history` | Throughput sequence over N timesteps |
| `rtt_history` | RTT sequence over N timesteps |
| `loss_history` | Loss rate sequence over N timesteps |

| Output | Description |
|--------|-------------|
| `predicted_bandwidth` | Predicted bandwidth (bytes/s) |
| `confidence` | Prediction confidence [0, 1] |

### AnomalyDetector — Traffic Anomaly Detection

```cpp
#include "neustack/ai/anomaly_model.hpp"
```

Autoencoder reconstruction error detection. Normal traffic has low reconstruction error; anomalous traffic has high reconstruction error.

```cpp
AnomalyDetector detector("models/anomaly_detector.onnx", 0.5f);

IAnomalyModel::Input input {
    .packets_rx_norm       = 0.3f,
    .packets_tx_norm       = 0.4f,
    .bytes_tx_norm         = 0.5f,
    .syn_rate_norm         = 0.1f,
    .rst_rate_norm         = 0.05f,
    .conn_established_norm = 0.6f,
    .tx_rx_ratio_norm      = 0.8f,
    .active_conn_norm      = 0.2f,
};

auto result = detector.infer(input);
if (result) {
    float error    = result->reconstruction_error;
    bool  anomaly  = result->is_anomaly;  // error > threshold
}

// 动态调整阈值
detector.set_threshold(0.8f);
```

| Input Features (8-dim) | Description |
|------------------------|-------------|
| `packets_rx_norm` | Received packet rate |
| `packets_tx_norm` | Sent packet rate |
| `bytes_tx_norm` | Sent byte rate |
| `syn_rate_norm` | SYN packet rate |
| `rst_rate_norm` | RST packet rate |
| `conn_established_norm` | Established connection count |
| `tx_rx_ratio_norm` | Send/receive ratio |
| `active_conn_norm` | Active connection count |

### SecurityAnomalyModel — Security Anomaly Detection

```cpp
#include "neustack/ai/security_model.hpp"
```

Deep Autoencoder dedicated to the firewall. Supports automatic threshold reading from model metadata.

```cpp
// threshold=0 → 从模型 metadata 读取
SecurityAnomalyModel sec_model("models/security_anomaly.onnx");

ISecurityModel::Input input {
    .pps_norm           = 0.3f,   // 包速率
    .bps_norm           = 0.4f,   // 字节速率
    .syn_rate_norm      = 0.8f,   // SYN 速率（高 = 可疑）
    .rst_rate_norm      = 0.1f,   // RST 速率
    .syn_ratio_norm     = 0.9f,   // SYN/SYN-ACK 比率
    .new_conn_rate_norm = 0.7f,   // 新连接速率
    .avg_pkt_size_norm  = 0.2f,   // 平均包大小
    .rst_ratio_norm     = 0.05f,  // RST/总包 比率
};

auto result = sec_model.infer(input);
if (result) {
    float error      = result->reconstruction_error;  // MSE
    bool  anomaly    = result->is_anomaly;
    float confidence = result->confidence;  // 离阈值越远越高
}

// 动态阈值
sec_model.set_threshold(0.02f);
float current = sec_model.get_threshold();
```

| Input Features (8-dim) | Description |
|------------------------|-------------|
| `pps_norm` | Packet rate (packets/s) |
| `bps_norm` | Byte rate (bytes/s) |
| `syn_rate_norm` | SYN packet rate |
| `rst_rate_norm` | RST packet rate |
| `syn_ratio_norm` | SYN / SYN-ACK ratio (high value = SYN flood indicator) |
| `new_conn_rate_norm` | New connection establishment rate |
| `avg_pkt_size_norm` | Average packet size |
| `rst_ratio_norm` | RST / total packet ratio |

| Output | Description |
|--------|-------------|
| `reconstruction_error` | MSE reconstruction error |
| `is_anomaly` | `error > threshold` |
| `confidence` | Confidence [0, 1], mapping of distance between error and threshold |

---

## NetworkAgent — Decision Layer

```cpp
#include "neustack/ai/ai_agent.hpp"
```

A policy state machine that coordinates all model outputs.

### State Machine

```
NORMAL ──(Bandwidth drops 50%)──→ DEGRADED
  │                            │
  │(Anomaly exceeds threshold)  (Anomaly exceeds threshold)
  ↓                            ↓
UNDER_ATTACK ──(50 consecutive normal)──→ RECOVERY ──(100 ticks stable)──→ NORMAL
```

| State | Meaning | Behavior |
|-------|---------|----------|
| `NORMAL` | Network is normal | Use Orca alpha directly |
| `DEGRADED` | Bandwidth dropped suddenly | clamp alpha ∈ [-0.3, 0.3] |
| `UNDER_ATTACK` | Anomaly detected | alpha forced = -0.5, reject new connections |
| `RECOVERY` | Recovering | clamp alpha ∈ [-0.1, 0.5] |

### Usage

```cpp
NetworkAgent agent(0.01f);  // 异常阈值

// 输入模型结果
agent.on_cwnd_adjust(0.3f);        // Orca 输出
agent.on_bw_prediction(5000000);   // 带宽预测 (bytes/s)
agent.on_anomaly(0.005f);          // 异常分数

// 查询决策
AgentState state = agent.state();           // 当前状态
float alpha      = agent.effective_alpha(); // 经过策略修正的 alpha
bool accept      = agent.should_accept_connection(); // 是否接受新连接

// 查询缓存值
float anomaly_score  = agent.anomaly_score();
uint32_t predicted   = agent.predicted_bw();
float raw_alpha      = agent.current_alpha();

// 决策日志
for (auto& d : agent.history()) {
    printf("[%lu] %s → %s: %s\n",
        d.timestamp_us,
        agent_state_name(d.from_state),
        agent_state_name(d.to_state),
        d.reason.c_str());
}
```

### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `DEFAULT_ANOMALY_THRESHOLD` | 0.01 | Default anomaly threshold |
| `ANOMALY_CLEAR_REQUIRED` | 50 | 50 consecutive normal results required to exit UNDER_ATTACK |
| `RECOVERY_TICKS_REQUIRED` | 100 | 100 stable ticks in RECOVERY before returning to NORMAL |
| `BW_DROP_RATIO` | 0.5 | Bandwidth dropping to 50% triggers DEGRADED |
| `BW_RECOVER_RATIO` | 0.85 | Recovery to 85% clears DEGRADED |
| `MAX_HISTORY_SIZE` | 1000 | Maximum decision log entries |

### effective_alpha Policy

```
State               alpha Handling
NORMAL              Use Orca raw output directly
DEGRADED            clamp(alpha, -0.3, 0.3)  — conservative adjustment
UNDER_ATTACK        Force -0.5               — actively shrink window
RECOVERY            clamp(alpha, -0.1, 0.5)  — cautious recovery
```

---

## Build Options

```bash
# 启用 AI（需要 ONNX Runtime）
cmake -B build -DNEUSTACK_ENABLE_AI=ON

# 下载 ONNX Runtime（如果没装）
./scripts/download/download_onnxruntime.sh
```

When enabled, the compiler defines the `NEUSTACK_AI_ENABLED` macro, making related headers and features available.

When AI is not enabled, model pointers in `IntelligencePlane` do not exist, and the intelligence plane is an empty shell.

## Model Files

| Model | Path | Training Docs |
|-------|------|---------------|
| Orca Actor | `models/orca_actor.onnx` | [ai-training.md](ai-training.md) |
| Bandwidth Prediction | `models/bandwidth_predictor.onnx` | [ai-training.md](ai-training.md) |
| Anomaly Detection | `models/anomaly_detector.onnx` | [ai-training.md](ai-training.md) |
| Security Anomaly | `models/security_anomaly.onnx` | [ai-training.md](ai-training.md) |

All models use the ONNX format and are exported after training in Python. See [AI Training Guide](ai-training.md) for the training process.
