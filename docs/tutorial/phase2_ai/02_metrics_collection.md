# 教程 02：指标采集与 TCP 埋点

> **前置要求**: 完成教程 01 (双线程架构)
> **目标**: 定义 TCPSample / GlobalMetrics 结构，在 TCP 关键路径埋入采集点

## 1. 概述

教程 01 建立了数据面/智能面双线程架构。本教程聚焦于：

1. **定义采集什么** — TCPSample (Per-ACK) 和 GlobalMetrics (全局计数器)
2. **定义在哪采集** — TCP 关键路径的埋点位置
3. **定义怎么用** — AI 特征提取 (OrcaFeatures 等)

```
┌─────────────────────────────────────────────────────────────────┐
│  本教程范围                                                      │
│                                                                  │
│  TCP 关键路径                      指标结构                       │
│  ┌─────────────────┐              ┌─────────────────┐           │
│  │ process_ack()   │─── push ───▶│ MetricsBuffer   │           │
│  │ handle_syn()    │              │ <TCPSample>     │           │
│  │ handle_rst()    │              └─────────────────┘           │
│  │ send_segment()  │                                            │
│  └────────┬────────┘              ┌─────────────────┐           │
│           │                       │ GlobalMetrics   │           │
│           └────── increment ─────▶│ (atomic)        │           │
│                                   └─────────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

## 2. TCPSample：Per-ACK 采样

### 2.1 结构定义

```cpp
// include/neustack/metrics/tcp_sample.hpp

struct TCPSample {
    // ─── 时间戳 (8 bytes) ───
    uint64_t timestamp_us;        // 采样时间戳 (微秒)

    // ─── RTT 相关 (12 bytes) ───
    uint32_t rtt_us;              // 本次 RTT 样本
    uint32_t min_rtt_us;          // 历史最小 RTT (基线延迟)
    uint32_t srtt_us;             // 平滑 RTT

    // ─── 拥塞窗口 (12 bytes) ───
    uint32_t cwnd;                // 当前 cwnd (MSS 单位)
    uint32_t ssthresh;            // 慢启动阈值
    uint32_t bytes_in_flight;     // 在途字节数

    // ─── 吞吐量 (8 bytes) ───
    uint32_t delivery_rate;       // 交付速率 (bytes/s)
    uint32_t send_rate;           // 发送速率 (bytes/s)

    // ─── 拥塞信号 (8 bytes, 含 padding) ───
    uint8_t loss_detected;        // 本周期是否检测到丢包
    uint8_t timeout_occurred;     // 本周期是否超时
    uint8_t ecn_ce_count;         // ECN: 本周期收到的 CE 标记包数
    uint8_t is_app_limited;       // 应用受限标志
    uint8_t _reserved[4];         // 对齐到 48 字节
};

static_assert(sizeof(TCPSample) == 48, "TCPSample should be 48 bytes");
```

### 2.2 字段说明

| 字段 | 来源 | AI 用途 |
|------|------|---------|
| `timestamp_us` | `now_us()` | 计算采样间隔 |
| `rtt_us` | ACK 到达时计算 | Orca: 排队延迟 |
| `min_rtt_us` | 历史最小值 | Orca: 基线延迟 |
| `cwnd` | 拥塞控制器 | Orca: 当前窗口 |
| `bytes_in_flight` | `snd_nxt - snd_una` | Orca: 管道填充度 |
| `delivery_rate` | 字节确认速率 | 带宽预测 |
| `loss_detected` | 三次重复 ACK | Orca: 丢包信号 |
| `ecn_ce_count` | IP 头 ECN 字段 | 早期拥塞信号 |
| `is_app_limited` | 发送缓冲区空 | 过滤无效带宽样本 |

### 2.3 派生指标

```cpp
// 排队延迟 = RTT - min_RTT (Orca 核心输入)
uint32_t queuing_delay_us() const {
    if (min_rtt_us == 0 || rtt_us < min_rtt_us) return 0;
    return rtt_us - min_rtt_us;
}

// RTT 比值 (归一化)
float rtt_ratio() const {
    if (min_rtt_us == 0) return 1.0f;
    return static_cast<float>(rtt_us) / min_rtt_us;
}

// 是否可用于带宽估计
bool is_valid_for_bw_estimation() const {
    return !is_app_limited && !timeout_occurred;
}
```

> **为什么需要 `is_app_limited`？**
>
> 当应用层没有数据可发送时，`delivery_rate` 不代表网络真实带宽，
> 而是应用的产出速率。BBR 等算法会过滤这类样本，避免低估带宽。

## 3. GlobalMetrics：全局计数器

### 3.1 结构定义

```cpp
// include/neustack/metrics/global_metrics.hpp

struct GlobalMetrics {
    // ─── 包统计 ───
    std::atomic<uint64_t> packets_rx{0};
    std::atomic<uint64_t> packets_tx{0};
    std::atomic<uint64_t> bytes_rx{0};
    std::atomic<uint64_t> bytes_tx{0};

    // ─── TCP 标志统计 (异常检测核心) ───
    std::atomic<uint64_t> syn_received{0};
    std::atomic<uint64_t> syn_ack_sent{0};
    std::atomic<uint64_t> rst_received{0};
    std::atomic<uint64_t> rst_sent{0};
    std::atomic<uint64_t> fin_received{0};

    // ─── 连接统计 ───
    std::atomic<uint32_t> active_connections{0};
    std::atomic<uint64_t> conn_established{0};
    std::atomic<uint64_t> conn_closed{0};
    std::atomic<uint64_t> conn_reset{0};
    std::atomic<uint64_t> conn_timeout{0};

    // 快照 (智能面读取用)
    struct Snapshot { /* 非原子副本 */ };
    Snapshot snapshot() const;
};

// 全局单例
inline GlobalMetrics& global_metrics() {
    static GlobalMetrics instance;
    return instance;
}
```

### 3.2 为什么用 atomic？

架构 B 中智能面在独立线程，需要跨线程读取：

```
数据面线程:  global_metrics().syn_received++     (写)
             ↓
智能面线程:  auto snap = global_metrics().snapshot()  (读)
```

**性能影响**: 无竞争的 `atomic<uint64_t>` 在 x86 上等同普通 increment，
因为只有一个写者，cache line 不会 bouncing。

### 3.3 Snapshot 机制

智能面不应该反复调用 atomic load，而是一次性拷贝出快照：

```cpp
struct Snapshot {
    uint64_t packets_rx;
    uint64_t syn_received;
    uint64_t rst_received;
    // ... 其他字段 ...

    struct Delta {
        uint64_t packets_rx;
        uint64_t syn_received;
        uint64_t rst_received;
        // ...
    };

    // 与上一次快照的差值 (计算速率)
    Delta diff(const Snapshot& prev) const;
};

Snapshot snapshot() const {
    return {
        .packets_rx = packets_rx.load(std::memory_order_relaxed),
        .syn_received = syn_received.load(std::memory_order_relaxed),
        // ...
    };
}
```

## 4. AI 特征向量

### 4.1 Orca 特征 (6 维)

```cpp
// include/neustack/metrics/ai_features.hpp

struct OrcaFeatures {
    float throughput_normalized;    // 吞吐量 / 估计带宽
    float queuing_delay_normalized; // 排队延迟 / min_RTT
    float rtt_ratio;                // RTT / min_RTT
    float loss_rate;                // 丢包率 [0, 1]
    float cwnd_normalized;          // cwnd / BDP
    float in_flight_ratio;          // in_flight / cwnd

    static OrcaFeatures from_sample(const TCPSample& s, uint32_t est_bw);
    std::vector<float> to_vector() const;
    static constexpr size_t dim() { return 6; }
};
```

### 4.2 异常检测特征 (5 维)

```cpp
struct AnomalyFeatures {
    float syn_rate;           // SYN/s
    float rst_rate;           // RST/s
    float new_conn_rate;      // 新连接/s
    float packet_rate;        // 包/s
    float avg_packet_size;    // 平均包大小

    static AnomalyFeatures from_delta(
        const GlobalMetrics::Snapshot::Delta& delta,
        double interval_sec
    );
    std::vector<float> to_vector() const;
    static constexpr size_t dim() { return 5; }
};
```

### 4.3 带宽预测特征 (时序)

```cpp
/**
 * 带宽预测特征
 *
 * 与 Orca/Anomaly 不同，这是时序输入：
 * 过去 N 个采样周期的历史数据，组成 LSTM 的输入序列
 */
struct BandwidthFeatures {
    std::vector<float> throughput_history;  // 历史吞吐量 (归一化)
    std::vector<float> rtt_history;         // 历史 RTT 比值
    std::vector<float> loss_history;        // 历史丢包率

    static BandwidthFeatures from_samples(
        const std::vector<TCPSample>& samples,
        uint32_t est_bw = 0
    );
    std::vector<float> to_vector() const;  // 展平: [throughput..., rtt..., loss...]
};
```

> **三种特征的区别**:
>
> | 特征 | 输入形状 | 频率 | 模型 |
> |------|----------|------|------|
> | OrcaFeatures | 6 维向量 | 每 10ms | DDPG (MLP) |
> | AnomalyFeatures | 5 维向量 | 每 1s | LSTM-AE |
> | BandwidthFeatures | N×3 时序 | 每 100ms | LSTM |

## 5. TCP 埋点位置

### 5.1 埋点总览

| 埋点位置 | 更新目标 | 频率 |
|----------|----------|------|
| `process_ack()` | `MetricsBuffer.push(sample)` | Per-ACK |
| `handle_segment()` 收到 SYN | `global_metrics().syn_received++` | Per-SYN |
| `handle_segment()` 收到 RST | `global_metrics().rst_received++` | Per-RST |
| `send_segment()` | `global_metrics().packets_tx++` | Per-Send |
| 连接进入 ESTABLISHED | `global_metrics().conn_established++` | Per-Conn |
| 连接关闭/超时 | `global_metrics().conn_closed++` | Per-Conn |

### 5.2 process_ack() 埋点

```cpp
// src/transport/tcp_connection.cpp

void TCPConnectionManager::process_ack(TCB* tcb, const TCPHeader& tcp) {
    // ... 现有的 ACK 处理逻辑 ...

    // ─── AI 指标采集 ───
    uint64_t now = now_us();
    if (now - tcb->last_sample_time_us >= TCB::SAMPLE_INTERVAL_US) {
        TCPSample sample;
        sample.timestamp_us = now;
        sample.rtt_us = tcb->srtt_us;
        sample.min_rtt_us = tcb->min_rtt_us;
        sample.srtt_us = tcb->srtt_us;
        sample.cwnd = tcb->cc ? tcb->cc->cwnd() / MSS : 1;
        sample.ssthresh = tcb->cc ? tcb->cc->ssthresh() : UINT32_MAX;
        sample.bytes_in_flight = tcb->snd_nxt - tcb->snd_una;
        sample.delivery_rate = tcb->delivery_rate;
        sample.send_rate = tcb->send_rate;
        sample.loss_detected = tcb->loss_this_period;
        sample.timeout_occurred = 0;
        sample.ecn_ce_count = tcb->ecn_ce_this_period;
        sample.is_app_limited = tcb->send_buffer.empty();

        _metrics_buffer.push(sample);
        tcb->last_sample_time_us = now;

        // 重置周期计数器
        tcb->loss_this_period = 0;
        tcb->ecn_ce_this_period = 0;
    }
}
```

### 5.3 handle_segment() 埋点

```cpp
void TCPConnectionManager::handle_segment(Packet& pkt) {
    auto& tcp = pkt.tcp_header();

    // 全局包统计
    global_metrics().packets_rx.fetch_add(1, std::memory_order_relaxed);
    global_metrics().bytes_rx.fetch_add(pkt.size(), std::memory_order_relaxed);

    // TCP 标志统计
    if (tcp.syn && !tcp.ack) {
        global_metrics().syn_received.fetch_add(1, std::memory_order_relaxed);
    }
    if (tcp.rst) {
        global_metrics().rst_received.fetch_add(1, std::memory_order_relaxed);
    }
    if (tcp.fin) {
        global_metrics().fin_received.fetch_add(1, std::memory_order_relaxed);
    }

    // ... 现有处理逻辑 ...
}
```

### 5.4 连接状态变化埋点

```cpp
void TCPConnectionManager::transition_state(TCB* tcb, TCPState new_state) {
    TCPState old_state = tcb->state;
    tcb->state = new_state;

    // 连接建立
    if (new_state == TCPState::ESTABLISHED && old_state != TCPState::ESTABLISHED) {
        global_metrics().active_connections.fetch_add(1, std::memory_order_relaxed);
        global_metrics().conn_established.fetch_add(1, std::memory_order_relaxed);
    }

    // 连接关闭
    if (new_state == TCPState::CLOSED || new_state == TCPState::TIME_WAIT) {
        if (old_state == TCPState::ESTABLISHED) {
            global_metrics().active_connections.fetch_sub(1, std::memory_order_relaxed);
        }
        global_metrics().conn_closed.fetch_add(1, std::memory_order_relaxed);
    }
}
```

## 6. TCB 扩展

在 TCB 中添加采样相关字段：

```cpp
// include/neustack/transport/tcp_tcb.hpp

struct TCB {
    // ... 现有字段 ...

    // ─── AI 采样控制 ───
    uint64_t last_sample_time_us = 0;
    static constexpr uint64_t SAMPLE_INTERVAL_US = 10000;  // 10ms

    // ─── 周期计数器 (每次采样后重置) ───
    uint8_t loss_this_period = 0;
    uint8_t ecn_ce_this_period = 0;

    // ─── 速率估计 ───
    uint32_t delivery_rate = 0;   // bytes/s
    uint32_t send_rate = 0;       // bytes/s
};
```

## 7. 新增文件清单

```
include/neustack/metrics/
├── tcp_sample.hpp        # TCPSample 结构
├── global_metrics.hpp    # GlobalMetrics (atomic)
└── ai_features.hpp       # OrcaFeatures, AnomalyFeatures
```

## 8. 下一步

- **教程 03: ONNX 集成** — 加载模型，在智能面线程中推理
- **教程 04: 异常检测** — LSTM-Autoencoder 实现

## 9. 参考资料

- [Orca Paper](https://www.usenix.org/conference/nsdi22/presentation/abbasloo) — 特征定义
- [BBR: Delivery Rate Estimation](https://datatracker.ietf.org/doc/html/draft-cardwell-iccrg-bbr-congestion-control) — delivery_rate 计算
- [RFC 3168: ECN](https://tools.ietf.org/html/rfc3168) — ECN 机制
