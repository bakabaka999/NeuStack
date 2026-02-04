# 教程 06：Orca 拥塞控制

> **前置要求**: 完成教程 01-05
> **目标**: 实现 CUBIC 和 Orca 拥塞控制算法（C++），并训练 DDPG 模型

本教程分为两大部分：
1. **C++ 实现**：在 transport 层实现 CUBIC 和 Orca 拥塞控制
2. **模型训练**：用 DDPG 强化学习训练 Orca 的 Actor 网络

---

## 1. Orca 原理

### 1.1 为什么用 RL 做拥塞控制

传统拥塞控制（CUBIC、BBR）是**手工设计的启发式算法**：
- 固定的参数（如 CUBIC 的 β=0.7）
- 无法适应所有网络环境
- 难以同时优化吞吐量、延迟、公平性

强化学习可以：
- 从数据中学习最优策略
- 自适应不同网络条件
- 平衡多个目标

### 1.2 Orca 的双层架构

**核心思想**：不完全替换 CUBIC，而是在其基础上做**调制**。

```
┌─────────────────────────────────────────────────────────┐
│                    Orca 双层控制                         │
│                                                         │
│   ┌─────────────┐                                       │
│   │   CUBIC     │ ──► cwnd_cubic (基础值)               │
│   │  (Layer 1)  │                                       │
│   └─────────────┘                                       │
│          │                                              │
│          ▼                                              │
│   ┌─────────────┐                                       │
│   │  DDPG RL    │ ──► α ∈ [-1, 1] (调制因子)            │
│   │  (Layer 2)  │                                       │
│   └─────────────┘                                       │
│          │                                              │
│          ▼                                              │
│   cwnd_final = 2^α × cwnd_cubic                         │
│                                                         │
│   α = -1  →  cwnd = 0.5 × cwnd_cubic  (激进降速)        │
│   α =  0  →  cwnd = 1.0 × cwnd_cubic  (保持 CUBIC)      │
│   α = +1  →  cwnd = 2.0 × cwnd_cubic  (激进加速)        │
└─────────────────────────────────────────────────────────┘
```

**优势**：
1. **安全性**：即使 RL 失效，还有 CUBIC 兜底
2. **稳定性**：α 的范围有限，不会做出极端决策
3. **可解释**：α 直观表示"比 CUBIC 激进/保守多少"

### 1.3 状态空间（7 维）

| 特征 | 含义 | 归一化 |
|------|------|--------|
| `throughput_normalized` | 当前吞吐量 | / max_throughput |
| `queuing_delay_normalized` | 排队延迟 (RTT - min_RTT) | / min_RTT |
| `rtt_ratio` | RTT / min_RTT | 已归一化 |
| `loss_rate` | 丢包率 | [0, 1] |
| `cwnd_normalized` | 当前 cwnd | / max_cwnd |
| `in_flight_ratio` | bytes_in_flight / (cwnd × MSS) | 已归一化 |
| `predicted_bw_normalized` | 带宽预测值 | / max_throughput |

第 7 维 `predicted_bw_normalized` 来自带宽预测模型，让 Orca 知道未来趋势。

### 1.4 动作空间

```
α ∈ [-1, 1]  (连续动作)

cwnd_new = 2^α × cwnd_cubic
```

### 1.5 奖励函数

Orca 使用**多目标奖励**：

```python
reward = throughput_reward - delay_penalty - loss_penalty

# 吞吐量奖励：鼓励高吞吐
throughput_reward = log(throughput + 1)

# 延迟惩罚：惩罚高排队延迟
delay_penalty = β × queuing_delay / min_rtt

# 丢包惩罚：惩罚丢包
loss_penalty = γ × loss_rate
```

参数 β、γ 控制目标权重：
- β 大 → 更注重低延迟
- γ 大 → 更注重避免丢包

---

## 2. CUBIC 回顾

### 2.1 CUBIC 核心公式

```
W(t) = C × (t - K)³ + W_max

其中：
- W(t): 时间 t 的 cwnd
- C: 缩放常数 (0.4)
- K: 到达 W_max 的时间
- W_max: 上次丢包时的 cwnd
```

### 2.2 NeuStack 中的 CUBIC

```cpp
// src/transport/tcp_congestion.cpp (简化)

void CubicCongestion::on_ack(uint32_t acked_bytes) {
    auto now = steady_clock::now();
    double t = duration<double>(now - _epoch_start).count();

    // CUBIC 窗口计算
    double K = cbrt(_w_max * (1 - BETA) / C);
    double w_cubic = C * pow(t - K, 3) + _w_max;

    // 取 CUBIC 和 Reno 的较大值
    double w_reno = _cwnd + (MSS * acked_bytes) / _cwnd;
    _cwnd = max(w_cubic, w_reno);
}

void CubicCongestion::on_loss() {
    _w_max = _cwnd;
    _cwnd = _cwnd * BETA;  // β = 0.7
    _epoch_start = steady_clock::now();
}
```

### 2.3 Orca 如何调制 CUBIC

```cpp
// Orca 调制后的 cwnd 更新
void OrcaCongestion::update_cwnd(float alpha) {
    // 1. 先让 CUBIC 计算基础值
    uint32_t cwnd_cubic = _cubic.get_cwnd();

    // 2. 用 RL 输出的 α 调制
    float multiplier = pow(2.0f, alpha);  // 2^α ∈ [0.5, 2.0]

    // 3. 计算最终 cwnd
    _cwnd = static_cast<uint32_t>(cwnd_cubic * multiplier);

    // 4. 限制范围
    _cwnd = clamp(_cwnd, MIN_CWND, MAX_CWND);
}
```

---

# Part 1: C++ 实现

---

## 3. CUBIC 拥塞控制实现

### 3.1 文件结构

```
include/neustack/transport/
├── tcp_tcb.hpp          # ICongestionControl 接口（已存在）
├── tcp_reno.hpp         # Reno 实现（已存在）
├── tcp_cubic.hpp        # CUBIC 实现（新增）
└── tcp_orca.hpp         # Orca 实现（新增）
```

### 3.2 tcp_cubic.hpp

```cpp
// include/neustack/transport/tcp_cubic.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_CUBIC_HPP
#define NEUSTACK_TRANSPORT_TCP_CUBIC_HPP

#include "neustack/transport/tcp_tcb.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>

namespace neustack {

/**
 * CUBIC 拥塞控制 (RFC 8312)
 *
 * 特点:
 * - 使用三次函数而非线性增长
 * - 对高带宽长延迟网络（BDP 大）更友好
 * - Linux 默认拥塞控制算法
 */
class TCPCubic : public ICongestionControl {
public:
    // CUBIC 常数
    static constexpr double C = 0.4;       // 缩放常数
    static constexpr double BETA = 0.7;    // 乘法减少因子
    static constexpr uint32_t MIN_CWND = 2;  // 最小 cwnd (MSS 单位)

    explicit TCPCubic(uint32_t mss = 1460)
        : _mss(mss < 536 ? 536 : mss)
        , _cwnd(10 * mss)              // 初始 10 MSS (RFC 6928)
        , _ssthresh(65535)
        , _w_max(0)
        , _k(0)
        , _epoch_start()
        , _in_slow_start(true)
        , _tcp_friendliness(true)
        , _w_tcp(0)
    {}

    void on_ack(uint32_t bytes_acked, uint32_t rtt_us) override {
        _last_rtt_us = rtt_us;

        if (_in_slow_start && _cwnd < _ssthresh) {
            // 慢启动：指数增长
            _cwnd += bytes_acked;
            return;
        }

        _in_slow_start = false;

        // 拥塞避免：CUBIC 增长
        auto now = std::chrono::steady_clock::now();

        // 首次进入拥塞避免，记录 epoch 开始时间
        if (_epoch_start.time_since_epoch().count() == 0) {
            _epoch_start = now;
            _w_tcp = _cwnd;  // TCP Reno 友好模式起点
        }

        // 计算自 epoch 开始的时间 (秒)
        double t = std::chrono::duration<double>(now - _epoch_start).count();

        // CUBIC 窗口目标
        double w_cubic = cubic_window(t);

        // TCP 友好模式 (确保不比 Reno 差)
        if (_tcp_friendliness) {
            // Reno 线性增长: W_tcp(t) = W_max * (1-β) + 3*β/(2-β) * t/RTT
            double rtt_sec = _last_rtt_us / 1e6;
            if (rtt_sec > 0) {
                _w_tcp += (3 * BETA / (2 - BETA)) * (t / rtt_sec) * _mss;
            }
            w_cubic = std::max(w_cubic, _w_tcp);
        }

        // 更新 cwnd
        uint32_t target = static_cast<uint32_t>(w_cubic);
        if (target > _cwnd) {
            // 增长：每个 ACK 增加 (target - cwnd) / cwnd
            uint32_t delta = (target - _cwnd) * _mss / _cwnd;
            _cwnd += std::max(delta, 1u);
        }

        // 上限
        _cwnd = std::min(_cwnd, 65535u * 16);
    }

    void on_loss(uint32_t bytes_lost) override {
        (void)bytes_lost;

        // 记录丢包时的窗口
        if (_cwnd < _w_max) {
            // 快速收敛：如果窗口比上次丢包时小，降低 W_max
            _w_max = _cwnd * (1 + BETA) / 2;
        } else {
            _w_max = _cwnd;
        }

        // 乘法减少
        _ssthresh = static_cast<uint32_t>(_cwnd * BETA);
        _ssthresh = std::max(_ssthresh, MIN_CWND * _mss);
        _cwnd = _ssthresh;

        // 计算 K: 到达 W_max 所需时间
        _k = std::cbrt(_w_max * (1 - BETA) / C);

        // 重置 epoch
        _epoch_start = std::chrono::steady_clock::time_point{};
        _in_slow_start = false;
    }

    void on_timeout() {
        // 超时更严重：重置到慢启动
        _ssthresh = static_cast<uint32_t>(_cwnd * BETA);
        _ssthresh = std::max(_ssthresh, MIN_CWND * _mss);
        _cwnd = _mss;  // 重置为 1 MSS
        _in_slow_start = true;
        _epoch_start = std::chrono::steady_clock::time_point{};
    }

    uint32_t cwnd() const override { return _cwnd; }
    uint32_t ssthresh() const override { return _ssthresh; }

    // 额外接口供 Orca 使用
    double w_max() const { return _w_max; }
    bool in_slow_start() const { return _in_slow_start; }

private:
    double cubic_window(double t) const {
        // W(t) = C * (t - K)^3 + W_max
        double dt = t - _k;
        return C * dt * dt * dt + _w_max;
    }

    uint32_t _mss;
    uint32_t _cwnd;
    uint32_t _ssthresh;

    // CUBIC 特有状态
    double _w_max;                                    // 上次丢包时的窗口
    double _k;                                        // 到达 W_max 的时间
    std::chrono::steady_clock::time_point _epoch_start;  // 当前增长周期开始时间
    bool _in_slow_start;

    // TCP 友好模式
    bool _tcp_friendliness;
    double _w_tcp;

    // RTT (用于 TCP 友好模式)
    uint32_t _last_rtt_us = 0;
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_CUBIC_HPP
```

### 3.3 CUBIC 参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| C | 0.4 | 缩放常数，控制增长速度 |
| β | 0.7 | 丢包后的乘法减少因子 |
| K | 计算得出 | 窗口恢复到 W_max 所需时间 |

**与 Reno 对比**：

```
Reno:  cwnd 线性增长，每 RTT +1 MSS
CUBIC: cwnd 三次函数增长，远离 W_max 时快速增长，接近时减慢

      cwnd
        │      CUBIC
        │      ╱────────────
        │    ╱    Reno
        │  ╱   ╱─────────
        │╱   ╱
        └───────────────── time
          ↑
        loss
```

---

## 4. Orca 拥塞控制实现

### 4.1 tcp_orca.hpp

Orca 基于 CUBIC，用 AI 模型输出的 α 调制 cwnd：

```cpp
// include/neustack/transport/tcp_orca.hpp

#ifndef NEUSTACK_TRANSPORT_TCP_ORCA_HPP
#define NEUSTACK_TRANSPORT_TCP_ORCA_HPP

#include "neustack/transport/tcp_cubic.hpp"
#include <cmath>
#include <functional>

namespace neustack {

/**
 * Orca 拥塞控制
 *
 * 双层架构:
 * - Layer 1: CUBIC 计算基础 cwnd
 * - Layer 2: AI 模型输出 α ∈ [-1, 1] 调制
 *
 * 最终 cwnd = 2^α × cwnd_cubic
 *
 * 参考: Orca: A Differential Congestion Control System (NSDI 2022)
 */
class TCPOrca : public ICongestionControl {
public:
    // α 的回调类型：输入当前状态，返回 α ∈ [-1, 1]
    // 状态: (throughput, rtt, min_rtt, loss_rate, cwnd, in_flight, predicted_bw)
    using AlphaCallback = std::function<float(
        float throughput_normalized,
        float queuing_delay_normalized,
        float rtt_ratio,
        float loss_rate,
        float cwnd_normalized,
        float in_flight_ratio,
        float predicted_bw_normalized
    )>;

    static constexpr uint32_t MAX_CWND = 65535 * 16;
    static constexpr uint32_t MIN_CWND_MSS = 2;

    explicit TCPOrca(
        uint32_t mss = 1460,
        AlphaCallback alpha_cb = nullptr
    )
        : _cubic(mss)
        , _mss(mss)
        , _alpha_callback(std::move(alpha_cb))
        , _current_alpha(0.0f)
        , _last_throughput(0)
        , _min_rtt_us(UINT32_MAX)
        , _bytes_in_flight(0)
        , _predicted_bw(0)
    {}

    void on_ack(uint32_t bytes_acked, uint32_t rtt_us) override {
        // 1. 让 CUBIC 先更新
        _cubic.on_ack(bytes_acked, rtt_us);

        // 2. 更新 RTT 统计
        if (rtt_us > 0 && rtt_us < _min_rtt_us) {
            _min_rtt_us = rtt_us;
        }
        _last_rtt_us = rtt_us;

        // 3. 吞吐量由 set_delivery_rate() 更新，此处不再估算

        // 4. 更新 in-flight
        if (_bytes_in_flight >= bytes_acked) {
            _bytes_in_flight -= bytes_acked;
        }

        // 5. 如果有 AI 回调，获取 α
        if (_alpha_callback) {
            _current_alpha = compute_alpha();
        }
        // 否则 α 保持上次设置的值 (通过 set_alpha)
    }

    void on_loss(uint32_t bytes_lost) override {
        _cubic.on_loss(bytes_lost);
        _loss_count++;
    }

    void on_timeout() {
        _cubic.on_timeout();
    }

    uint32_t cwnd() const override {
        // 应用 α 调制
        uint32_t cwnd_cubic = _cubic.cwnd();
        float multiplier = std::pow(2.0f, _current_alpha);
        uint32_t cwnd_orca = static_cast<uint32_t>(cwnd_cubic * multiplier);

        // 限制范围
        return std::clamp(cwnd_orca, MIN_CWND_MSS * _mss, MAX_CWND);
    }

    uint32_t ssthresh() const override {
        return _cubic.ssthresh();
    }

    // ─── Orca 特有接口 ───

    // 设置 α (供 IntelligencePlane 调用)
    void set_alpha(float alpha) {
        _current_alpha = std::clamp(alpha, -1.0f, 1.0f);
    }

    float alpha() const { return _current_alpha; }

    // 获取 CUBIC 的基础 cwnd
    uint32_t cwnd_cubic() const { return _cubic.cwnd(); }

    // 设置带宽预测值 (供 IntelligencePlane 调用)
    void set_predicted_bandwidth(uint64_t bw) {
        _predicted_bw = bw;
    }

    // 设置实际吞吐量 (供协议栈在收到 ACK 时调用)
    void set_delivery_rate(uint64_t rate) {
        _last_throughput = rate;
    }

    // 记录发送
    void on_send(uint32_t bytes) {
        _bytes_in_flight += bytes;
        _packets_sent++;
    }

    // 重置采样周期统计
    void reset_period_stats() {
        _loss_count = 0;
        _packets_sent = 0;
    }

    // ─── 状态获取（供 AI 采样）───

    uint64_t throughput() const { return _last_throughput; }
    uint32_t rtt_us() const { return _last_rtt_us; }
    uint32_t min_rtt_us() const { return _min_rtt_us; }
    uint32_t bytes_in_flight() const { return _bytes_in_flight; }
    uint64_t predicted_bw() const { return _predicted_bw; }

    float loss_rate() const {
        return _packets_sent > 0
            ? static_cast<float>(_loss_count) / _packets_sent
            : 0.0f;
    }

private:
    float compute_alpha() {
        // 归一化参数
        constexpr float MAX_THROUGHPUT = 100e6f;  // 100 MB/s
        constexpr float MAX_CWND = 65535.0f;

        float throughput_norm = std::min(_last_throughput / MAX_THROUGHPUT, 1.0f);
        float queuing_delay = (_min_rtt_us > 0 && _last_rtt_us > _min_rtt_us)
            ? static_cast<float>(_last_rtt_us - _min_rtt_us) / _min_rtt_us
            : 0.0f;
        float rtt_ratio = (_min_rtt_us > 0)
            ? static_cast<float>(_last_rtt_us) / _min_rtt_us
            : 1.0f;
        float cwnd_norm = static_cast<float>(_cubic.cwnd()) / MAX_CWND;
        float in_flight_ratio = (_cubic.cwnd() > 0)
            ? static_cast<float>(_bytes_in_flight) / (_cubic.cwnd())
            : 0.0f;
        float predicted_bw_norm = std::min(_predicted_bw / MAX_THROUGHPUT, 1.0f);

        return _alpha_callback(
            throughput_norm,
            queuing_delay,
            rtt_ratio,
            loss_rate(),
            cwnd_norm,
            in_flight_ratio,
            predicted_bw_norm
        );
    }

    TCPCubic _cubic;
    uint32_t _mss;

    // AI 回调
    AlphaCallback _alpha_callback;
    float _current_alpha;

    // 状态
    uint64_t _last_throughput;
    uint32_t _last_rtt_us = 0;
    uint32_t _min_rtt_us;
    uint32_t _bytes_in_flight;
    uint64_t _predicted_bw;

    // 采样周期统计
    uint32_t _loss_count = 0;
    uint32_t _packets_sent = 0;
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_ORCA_HPP
```

### 4.2 Orca 使用方式

NeuStack 的 TCP 采用 `TCPConnectionManager` 管理所有连接，每个连接的状态存储在 `TCB` (Transmission Control Block) 中。
拥塞控制器通过 `tcb->congestion_control` 指针访问。

**方式一：手动设置 α（推荐）**

通过 AI 智能面输出的 `AIAction` 来设置 α：

```cpp
// TCPLayer 在 process_ai_actions() 中处理 AI 决策
void apply_cwnd_action(TCB* tcb, const AIAction& action) {
    // 将 ICongestionControl 指针转换为 TCPOrca
    auto* orca = dynamic_cast<TCPOrca*>(tcb->congestion_control.get());
    if (orca) {
        orca->set_alpha(action.cwnd.alpha);
    }
}
```

**方式二：回调模式（自包含推理）**

如果希望 Orca 内部直接调用 ONNX 模型，可以在创建时传入回调：

```cpp
// 在 TCPConnectionManager::create_tcb() 中
auto orca = std::make_unique<TCPOrca>(tcb->options.mss, [&model](
    float throughput, float queuing_delay, float rtt_ratio,
    float loss_rate, float cwnd, float in_flight, float predicted_bw
) -> float {
    std::vector<float> features = {
        throughput, queuing_delay, rtt_ratio,
        loss_rate, cwnd, in_flight, predicted_bw
    };
    auto result = model->run(features);
    return result[0];  // α ∈ [-1, 1]
});

tcb->congestion_control = std::move(orca);
```

> **注意**：方式一更灵活，因为 AI 模型由独立的 `IntelligencePlane` 线程管理，
> 不会阻塞 TCP 数据路径。方式二适合简单场景或测试。

### 4.3 与 TCPConnectionManager 集成

NeuStack 的实际架构是：

```
TCPLayer                    TCPConnectionManager              TCB
────────────────────────────────────────────────────────────────────
  │                              │                             │
  ├── _tcp_mgr ─────────────────►│                             │
  │                              ├── _connections ────────────►│
  │                              │   map<TCPTuple, unique_ptr<TCB>>
  │                              │                             │
  ├── _metrics_buf ──────────────┼────────────────────────────►│ (采样)
  │   MetricsBuffer<TCPSample>   │                             │
  │                              │                             │
  ├── _action_queue ◄────────────┼─────────────────────────────│
  │   SPSCQueue<AIAction>        │                             │
  │                              │                             │
  └── process_ai_actions() ─────►│ apply_cwnd_action() ───────►│
                                 │                    tcb->congestion_control
```

**拥塞控制算法在 TCB 创建时设置：**

```cpp
// src/transport/tcp_connection.cpp 中的 create_tcb()

TCB *TCPConnectionManager::create_tcb(const TCPTuple &t_tuple) {
    auto tcb = std::make_unique<TCB>();
    tcb->t_tuple = t_tuple;
    tcb->state = TCPState::CLOSED;
    tcb->last_activity = std::chrono::steady_clock::now();

    // 应用默认选项
    tcb->apply_options(_default_options);

    // 创建拥塞控制器（默认使用 Reno）
    // 如需使用 Orca，修改此处：
    // tcb->congestion_control = std::make_unique<TCPOrca>(tcb->options.mss);
    tcb->congestion_control = std::make_unique<TCPReno>(tcb->options.mss);

    TCB *ptr = tcb.get();
    _connections[t_tuple] = std::move(tcb);
    return ptr;
}
```

**AI 指标采集在 process_ack() 中进行：**

```cpp
// src/transport/tcp_connection.cpp 中的 process_ack() (已实现)

void TCPConnectionManager::process_ack(TCB *tcb, uint32_t ack_num, uint16_t window) {
    // ... 前面是 ACK 处理逻辑 ...

    // ─── AI 指标采集: 定期推送 TCPSample ───
    if (_metrics_buf) {
        auto now = std::chrono::steady_clock::now();
        uint64_t now_us = /* ... */;

        if (now_us - tcb->last_sample_time_us >= TCB::SAMPLE_INTERVAL_US) {
            TCPSample sample{};
            sample.timestamp_us = now_us;
            sample.rtt_us = tcb->srtt_us;
            sample.min_rtt_us = tcb->min_rtt_us;
            sample.cwnd = tcb->congestion_control
                        ? tcb->congestion_control->cwnd() / tcb->options.mss
                        : 1;
            sample.bytes_in_flight = tcb->snd_nxt - tcb->snd_una;
            sample.packets_sent = tcb->packets_sent_period;
            sample.packets_lost = tcb->packets_lost_period;
            // ... 其他字段 ...

            _metrics_buf->push(sample);  // 无锁推送到 AI 线程

            // 重置周期计数器
            tcb->last_sample_time_us = now_us;
            tcb->packets_sent_period = 0;
            tcb->packets_lost_period = 0;
        }
    }
}
```

### 4.4 TCPLayer 与 IntelligencePlane 集成

**TCPLayer 是 TCP 的高层接口，负责：**
1. 管理 `TCPConnectionManager`
2. 提供 Stream 抽象（`TCPStreamConnection`）
3. 连接 AI 智能面（`IntelligencePlane`）

**数据流向：**

```
TCP 数据面线程                          AI 智能面线程
────────────────────                   ────────────────────
TCPConnectionManager                   IntelligencePlane
       │                                      │
       ├── process_ack()                      │
       │   └── TCPSample ──────────────────►  │ (无锁)
       │       push to _metrics_buf           │
       │                                      ├── process_orca()
       │                                      │   └── ONNX 推理
       │                                      │
       │                                      ├── AIAction
       │       _action_queue  ◄───────────────┘ (无锁)
       │
TCPLayer::on_timer()
       │
       └── process_ai_actions()
           └── apply_cwnd_action()
               └── tcb->congestion_control->set_alpha()
```

**TCPLayer 中的 AI 集成代码（已实现）：**

```cpp
// include/neustack/transport/tcp_layer.hpp

class TCPLayer {
private:
    // AI 通信通道
    MetricsBuffer<TCPSample, 1024> _metrics_buf;  // TCP → AI
    SPSCQueue<AIAction, 16> _action_queue;        // AI → TCP

    std::unique_ptr<IntelligencePlane> _ai;

    // 处理 AI 决策（在 on_timer() 中调用）
    void process_ai_actions() {
        AIAction action;
        while (_action_queue.try_pop(action)) {
            switch (action.type) {
                case AIAction::Type::CWND_ADJUST:
                    apply_cwnd_action(action);
                    break;
                case AIAction::Type::ANOMALY_ALERT:
                    handle_anomaly(action);
                    break;
                default:
                    break;
            }
        }
    }

    // 应用 cwnd 调整（需要实现）
    void apply_cwnd_action(const AIAction& action) {
        // TODO: 根据 action.conn_id 找到对应 TCB，调整 cwnd
        // 目前简化版：遍历所有连接
        for (auto& [tuple, tcb] : _tcp_mgr._connections) {
            auto* orca = dynamic_cast<TCPOrca*>(tcb->congestion_control.get());
            if (orca) {
                orca->set_alpha(action.cwnd.alpha);

                // 可选：同步带宽预测
                // orca->set_predicted_bandwidth(action.bandwidth.predicted_bw);
            }
        }
    }
};
```

**启用 AI：**

```cpp
// 应用层代码
TCPLayer tcp(ip_layer, local_ip);

IntelligencePlaneConfig config;
config.orca_model_path = "models/orca_actor.onnx";
config.anomaly_model_path = "models/anomaly_detector.onnx";
config.bandwidth_model_path = "models/bandwidth_predictor.onnx";

tcp.enable_ai(config);  // 启动 AI 线程
```

**完整的 AI 集成流程：**

1. `TCPLayer` 构造时，将 `_metrics_buf` 传给 `TCPConnectionManager`
2. `enable_ai()` 创建 `IntelligencePlane`，传入 `_metrics_buf` 和 `_action_queue`
3. TCP 数据处理（收发包）在主线程，`process_ack()` 中采集 `TCPSample`
4. AI 线程周期性从 `_metrics_buf` 读取样本，运行 ONNX 推理
5. AI 输出 `AIAction` 推送到 `_action_queue`
6. `TCPLayer::on_timer()` 调用 `process_ai_actions()` 消费动作
7. `apply_cwnd_action()` 找到对应 TCB，设置 Orca 的 α

---

# Part 2: 模型训练

---

## 5. DDPG 强化学习

### 5.1 为什么用 DDPG

| 算法 | 动作空间 | 优点 | 缺点 |
|------|---------|------|------|
| DQN | 离散 | 简单 | 不适合连续动作 |
| **DDPG** | 连续 | 适合 α ∈ [-1,1] | 需要调参 |
| PPO | 连续 | 稳定 | 样本效率低 |
| SAC | 连续 | 稳定+高效 | 复杂 |

DDPG (Deep Deterministic Policy Gradient) 适合连续动作空间，且实现相对简单。

### 5.2 Actor-Critic 架构

```
┌─────────────────────────────────────────────────────────┐
│                      DDPG 架构                           │
│                                                         │
│   状态 s ──┬──► Actor(s) ──► 动作 a (α)                 │
│            │                    │                       │
│            │                    ▼                       │
│            └──► Critic(s, a) ──► Q(s, a) (价值)         │
│                                                         │
│   Actor:  学习策略 π(s) → a                             │
│   Critic: 评估动作价值 Q(s, a)                          │
└─────────────────────────────────────────────────────────┘
```

### 5.3 训练流程

```
1. 收集经验 (s, a, r, s') 存入 Replay Buffer
2. 从 Buffer 采样 batch
3. 更新 Critic:
   - target_Q = r + γ × Critic_target(s', Actor_target(s'))
   - loss = MSE(Critic(s, a), target_Q)
4. 更新 Actor:
   - loss = -mean(Critic(s, Actor(s)))  # 最大化 Q 值
5. 软更新 target 网络:
   - θ_target = τ×θ + (1-τ)×θ_target
```

---

## 6. 环境准备

### 6.1 目录结构

```
NeuStack/
├── training/
│   ├── orca/
│   │   ├── env.py              # 模拟环境
│   │   ├── network.py          # Actor/Critic 网络
│   │   ├── ddpg.py             # DDPG 算法
│   │   ├── replay_buffer.py    # 经验回放
│   │   ├── train.py            # 训练脚本
│   │   ├── export_onnx.py      # 导出 ONNX
│   │   └── config.yaml         # 配置
│   ├── anomaly/
│   └── bandwidth/
└── models/
    └── orca_actor.onnx
```

### 6.2 依赖

```bash
# 与之前相同，无需额外安装
pip install torch numpy pyyaml onnx onnxruntime matplotlib
```

---

## 7. 模拟环境

### 7.1 网络模拟器

由于我们没有真实网络环境，需要构建一个**简化的网络模拟器**：

```python
# training/orca/env.py

import numpy as np
from dataclasses import dataclass
from typing import Tuple, Optional

@dataclass
class NetworkState:
    """网络状态"""
    throughput: float       # bytes/s
    rtt: float             # seconds
    min_rtt: float         # seconds
    loss_rate: float       # [0, 1]
    cwnd: int              # packets
    bytes_in_flight: int   # bytes
    predicted_bw: float    # bytes/s (来自带宽预测)


class SimpleNetworkEnv:
    """
    简化的网络模拟环境

    模拟一个瓶颈链路，带有：
    - 可变带宽
    - 排队延迟
    - 丢包
    """

    def __init__(
        self,
        bandwidth_mbps: float = 100.0,      # 瓶颈带宽 Mbps
        base_rtt_ms: float = 20.0,          # 基础 RTT ms
        buffer_size_packets: int = 100,     # 瓶颈缓冲区大小
        mss: int = 1460,                    # MSS
        max_cwnd: int = 1000,               # 最大 cwnd
        episode_steps: int = 200,           # 每 episode 步数
        bandwidth_variation: bool = True,   # 是否模拟带宽波动
    ):
        self.bandwidth = bandwidth_mbps * 1e6 / 8  # bytes/s
        self.base_rtt = base_rtt_ms / 1000  # seconds
        self.buffer_size = buffer_size_packets
        self.mss = mss
        self.max_cwnd = max_cwnd
        self.episode_steps = episode_steps
        self.bandwidth_variation = bandwidth_variation

        # 归一化参数
        self.max_throughput = 100e6  # 100 MB/s
        self.max_delay = 0.1         # 100ms

        self.reset()

    def reset(self) -> np.ndarray:
        """重置环境，返回初始状态"""
        self.step_count = 0
        self.cwnd = 10  # 初始 cwnd
        self.bytes_in_flight = 0
        self.queue_size = 0  # 当前队列大小

        # CUBIC 状态
        self.w_max = self.cwnd
        self.epoch_start = 0

        # 带宽变化
        self._init_bandwidth_pattern()

        return self._get_obs()

    def _init_bandwidth_pattern(self):
        """初始化带宽变化模式"""
        if self.bandwidth_variation:
            # 生成一个随机的带宽变化序列
            t = np.linspace(0, 4 * np.pi, self.episode_steps)
            # 基础带宽 + 正弦波动 + 随机突变
            self.bandwidth_pattern = (
                self.bandwidth * (0.7 + 0.3 * np.sin(t)) +
                np.random.normal(0, self.bandwidth * 0.1, self.episode_steps)
            )
            self.bandwidth_pattern = np.clip(
                self.bandwidth_pattern,
                self.bandwidth * 0.3,
                self.bandwidth * 1.2
            )
        else:
            self.bandwidth_pattern = np.full(self.episode_steps, self.bandwidth)

    def _get_current_bandwidth(self) -> float:
        """获取当前时刻的带宽"""
        idx = min(self.step_count, len(self.bandwidth_pattern) - 1)
        return self.bandwidth_pattern[idx]

    def _compute_cubic_cwnd(self) -> int:
        """计算 CUBIC 的 cwnd 建议值"""
        C = 0.4
        beta = 0.7
        t = (self.step_count - self.epoch_start) * 0.01  # 假设 10ms 一步

        if self.w_max > 0:
            K = ((self.w_max * (1 - beta)) / C) ** (1/3)
            w_cubic = C * (t - K) ** 3 + self.w_max
        else:
            w_cubic = self.cwnd + 1

        # Reno 线性增长
        w_reno = self.cwnd + 1

        return int(max(w_cubic, w_reno, 1))

    def step(self, alpha: float) -> Tuple[np.ndarray, float, bool, dict]:
        """
        执行一步

        Args:
            alpha: RL 输出的调制因子 ∈ [-1, 1]

        Returns:
            obs: 新状态
            reward: 奖励
            done: 是否结束
            info: 额外信息
        """
        self.step_count += 1

        # 1. 获取当前带宽
        current_bw = self._get_current_bandwidth()

        # 2. 计算 CUBIC cwnd 并应用 α 调制
        cwnd_cubic = self._compute_cubic_cwnd()
        multiplier = 2 ** alpha  # ∈ [0.5, 2.0]
        new_cwnd = int(cwnd_cubic * multiplier)
        new_cwnd = np.clip(new_cwnd, 1, self.max_cwnd)

        # 3. 计算发送速率和排队
        send_rate = (new_cwnd * self.mss) / self.base_rtt  # bytes/s

        # 排队 = 发送速率超过带宽的部分
        excess_rate = max(0, send_rate - current_bw)
        queue_growth = excess_rate * 0.01  # 10ms 的累积
        self.queue_size = min(self.queue_size + queue_growth / self.mss, self.buffer_size)

        # 队列排空
        drain_rate = current_bw * 0.01 / self.mss
        self.queue_size = max(0, self.queue_size - drain_rate)

        # 4. 计算丢包率
        if self.queue_size >= self.buffer_size * 0.9:
            loss_rate = 0.1 + 0.4 * (self.queue_size / self.buffer_size - 0.9) / 0.1
        elif self.queue_size >= self.buffer_size * 0.5:
            loss_rate = 0.01 + 0.09 * (self.queue_size / self.buffer_size - 0.5) / 0.4
        else:
            loss_rate = 0.001
        loss_rate = np.clip(loss_rate, 0, 0.5)

        # 5. 计算实际吞吐量
        throughput = min(send_rate, current_bw) * (1 - loss_rate)

        # 6. 计算 RTT
        queuing_delay = (self.queue_size * self.mss) / current_bw if current_bw > 0 else 0
        rtt = self.base_rtt + queuing_delay

        # 7. 处理丢包事件 (更新 CUBIC 状态)
        if loss_rate > 0.05:
            self.w_max = new_cwnd
            self.epoch_start = self.step_count

        # 8. 更新状态
        self.cwnd = new_cwnd
        self.bytes_in_flight = int(new_cwnd * self.mss * 0.8)  # 假设 80% 在飞

        # 9. 计算奖励
        reward = self._compute_reward(throughput, rtt, loss_rate, current_bw)

        # 10. 检查是否结束
        done = self.step_count >= self.episode_steps

        # 11. 额外信息
        info = {
            'throughput': throughput,
            'rtt': rtt,
            'loss_rate': loss_rate,
            'cwnd': self.cwnd,
            'cwnd_cubic': cwnd_cubic,
            'alpha': alpha,
            'bandwidth': current_bw,
            'queue_size': self.queue_size,
        }

        return self._get_obs(), reward, done, info

    def _compute_reward(
        self,
        throughput: float,
        rtt: float,
        loss_rate: float,
        bandwidth: float
    ) -> float:
        """计算奖励"""
        # 吞吐量奖励 (归一化到 [0, 1])
        throughput_reward = throughput / bandwidth if bandwidth > 0 else 0

        # 延迟惩罚
        queuing_delay = rtt - self.base_rtt
        delay_penalty = 2.0 * (queuing_delay / self.base_rtt)

        # 丢包惩罚
        loss_penalty = 10.0 * loss_rate

        reward = throughput_reward - delay_penalty - loss_penalty

        return reward

    def _get_obs(self) -> np.ndarray:
        """获取当前观测（7 维状态）"""
        current_bw = self._get_current_bandwidth()

        # 估算当前状态
        send_rate = (self.cwnd * self.mss) / self.base_rtt
        throughput = min(send_rate, current_bw)
        queuing_delay = (self.queue_size * self.mss) / current_bw if current_bw > 0 else 0
        rtt = self.base_rtt + queuing_delay

        # 简单估计丢包率
        loss_rate = 0.001 if self.queue_size < self.buffer_size * 0.5 else 0.05

        # 带宽预测 (这里简化为当前带宽，实际应该用模型预测)
        predicted_bw = current_bw

        obs = np.array([
            throughput / self.max_throughput,                    # throughput_normalized
            queuing_delay / self.base_rtt,                       # queuing_delay_normalized
            rtt / self.base_rtt,                                 # rtt_ratio
            loss_rate,                                           # loss_rate
            self.cwnd / self.max_cwnd,                           # cwnd_normalized
            self.bytes_in_flight / (self.cwnd * self.mss + 1),   # in_flight_ratio
            predicted_bw / self.max_throughput,                  # predicted_bw_normalized
        ], dtype=np.float32)

        return obs

    @property
    def state_dim(self) -> int:
        return 7

    @property
    def action_dim(self) -> int:
        return 1


class MultiFlowEnv:
    """
    多流竞争环境

    模拟多个 TCP 流竞争同一瓶颈带宽
    """

    def __init__(
        self,
        num_flows: int = 2,
        bandwidth_mbps: float = 100.0,
        base_rtt_ms: float = 20.0,
        **kwargs
    ):
        self.num_flows = num_flows
        self.bandwidth = bandwidth_mbps * 1e6 / 8

        # 为每个流创建独立状态
        self.flows = [
            {
                'cwnd': 10,
                'bytes_in_flight': 0,
                'w_max': 10,
                'epoch_start': 0,
            }
            for _ in range(num_flows)
        ]

        self.base_rtt = base_rtt_ms / 1000
        self.buffer_size = kwargs.get('buffer_size_packets', 100)
        self.mss = kwargs.get('mss', 1460)
        self.max_cwnd = kwargs.get('max_cwnd', 1000)
        self.max_throughput = 100e6

        self.step_count = 0
        self.episode_steps = kwargs.get('episode_steps', 200)
        self.queue_size = 0

    def reset(self) -> np.ndarray:
        """重置，返回第一个流的状态"""
        self.step_count = 0
        self.queue_size = 0

        for flow in self.flows:
            flow['cwnd'] = 10
            flow['bytes_in_flight'] = 0
            flow['w_max'] = 10
            flow['epoch_start'] = 0

        return self._get_obs(0)

    def step(self, alpha: float, flow_idx: int = 0) -> Tuple[np.ndarray, float, bool, dict]:
        """
        执行一步（只控制指定流，其他流用 CUBIC）
        """
        self.step_count += 1

        # 所有流的发送速率总和
        total_send_rate = 0
        flow_send_rates = []

        for i, flow in enumerate(self.flows):
            if i == flow_idx:
                # 被 RL 控制的流
                cwnd_cubic = self._compute_cubic_cwnd(flow)
                multiplier = 2 ** alpha
                flow['cwnd'] = int(np.clip(cwnd_cubic * multiplier, 1, self.max_cwnd))
            else:
                # 其他流用纯 CUBIC
                flow['cwnd'] = self._compute_cubic_cwnd(flow)
                flow['cwnd'] = int(np.clip(flow['cwnd'], 1, self.max_cwnd))

            send_rate = (flow['cwnd'] * self.mss) / self.base_rtt
            flow_send_rates.append(send_rate)
            total_send_rate += send_rate

        # 带宽公平分配
        if total_send_rate > self.bandwidth:
            # 按比例分配
            throughputs = [
                r / total_send_rate * self.bandwidth
                for r in flow_send_rates
            ]
        else:
            throughputs = flow_send_rates

        # 更新队列
        excess = max(0, total_send_rate - self.bandwidth) * 0.01
        self.queue_size = min(self.queue_size + excess / self.mss, self.buffer_size)
        self.queue_size = max(0, self.queue_size - self.bandwidth * 0.01 / self.mss)

        # 计算丢包率（所有流共享）
        if self.queue_size >= self.buffer_size * 0.8:
            loss_rate = 0.1
        else:
            loss_rate = 0.001

        # 更新 CUBIC 状态
        if loss_rate > 0.05:
            for flow in self.flows:
                flow['w_max'] = flow['cwnd']
                flow['epoch_start'] = self.step_count

        # 计算 RTT
        queuing_delay = (self.queue_size * self.mss) / self.bandwidth
        rtt = self.base_rtt + queuing_delay

        # 被控制流的奖励
        my_throughput = throughputs[flow_idx] * (1 - loss_rate)

        # 奖励：吞吐 - 延迟惩罚 - 丢包惩罚 + 公平性奖励
        reward = (
            my_throughput / self.bandwidth -
            2.0 * queuing_delay / self.base_rtt -
            10.0 * loss_rate
        )

        # 公平性奖励（Jain's fairness index）
        if len(throughputs) > 1:
            fairness = (sum(throughputs) ** 2) / (len(throughputs) * sum(t**2 for t in throughputs))
            reward += 0.5 * (fairness - 0.5)  # 鼓励公平

        done = self.step_count >= self.episode_steps

        info = {
            'throughput': my_throughput,
            'rtt': rtt,
            'loss_rate': loss_rate,
            'cwnd': self.flows[flow_idx]['cwnd'],
            'all_throughputs': throughputs,
        }

        return self._get_obs(flow_idx), reward, done, info

    def _compute_cubic_cwnd(self, flow: dict) -> int:
        C = 0.4
        beta = 0.7
        t = (self.step_count - flow['epoch_start']) * 0.01

        if flow['w_max'] > 0:
            K = ((flow['w_max'] * (1 - beta)) / C) ** (1/3)
            w_cubic = C * (t - K) ** 3 + flow['w_max']
        else:
            w_cubic = flow['cwnd'] + 1

        return int(max(w_cubic, flow['cwnd'] + 1, 1))

    def _get_obs(self, flow_idx: int) -> np.ndarray:
        flow = self.flows[flow_idx]

        send_rate = (flow['cwnd'] * self.mss) / self.base_rtt
        throughput = min(send_rate, self.bandwidth / self.num_flows)
        queuing_delay = (self.queue_size * self.mss) / self.bandwidth
        rtt = self.base_rtt + queuing_delay
        loss_rate = 0.001 if self.queue_size < self.buffer_size * 0.5 else 0.05

        return np.array([
            throughput / self.max_throughput,
            queuing_delay / self.base_rtt,
            rtt / self.base_rtt,
            loss_rate,
            flow['cwnd'] / self.max_cwnd,
            0.8,  # in_flight_ratio
            self.bandwidth / self.num_flows / self.max_throughput,  # predicted_bw
        ], dtype=np.float32)

    @property
    def state_dim(self) -> int:
        return 7

    @property
    def action_dim(self) -> int:
        return 1
```

---

## 8. 网络架构

### 6.1 Actor 和 Critic 网络

```python
# training/orca/network.py

import torch
import torch.nn as nn
import torch.nn.functional as F


class Actor(nn.Module):
    """
    Actor 网络：状态 → 动作 (α)

    输出 tanh 激活，确保 α ∈ [-1, 1]
    """

    def __init__(
        self,
        state_dim: int = 7,
        hidden_dims: list = [256, 256],
        action_dim: int = 1
    ):
        super().__init__()

        layers = []
        prev_dim = state_dim

        for hidden_dim in hidden_dims:
            layers.append(nn.Linear(prev_dim, hidden_dim))
            layers.append(nn.ReLU())
            prev_dim = hidden_dim

        self.net = nn.Sequential(*layers)
        self.out = nn.Linear(prev_dim, action_dim)

    def forward(self, state: torch.Tensor) -> torch.Tensor:
        """
        Args:
            state: [batch, state_dim]
        Returns:
            action: [batch, action_dim], 范围 [-1, 1]
        """
        x = self.net(state)
        action = torch.tanh(self.out(x))
        return action


class Critic(nn.Module):
    """
    Critic 网络：(状态, 动作) → Q 值
    """

    def __init__(
        self,
        state_dim: int = 7,
        action_dim: int = 1,
        hidden_dims: list = [256, 256]
    ):
        super().__init__()

        layers = []
        prev_dim = state_dim + action_dim

        for hidden_dim in hidden_dims:
            layers.append(nn.Linear(prev_dim, hidden_dim))
            layers.append(nn.ReLU())
            prev_dim = hidden_dim

        layers.append(nn.Linear(prev_dim, 1))

        self.net = nn.Sequential(*layers)

    def forward(self, state: torch.Tensor, action: torch.Tensor) -> torch.Tensor:
        """
        Args:
            state: [batch, state_dim]
            action: [batch, action_dim]
        Returns:
            q_value: [batch, 1]
        """
        x = torch.cat([state, action], dim=-1)
        return self.net(x)


class OrcaActor(nn.Module):
    """
    轻量版 Actor（用于部署）

    更小的网络，适合实时推理
    """

    def __init__(self, state_dim: int = 7, hidden_dim: int = 64):
        super().__init__()

        self.net = nn.Sequential(
            nn.Linear(state_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, hidden_dim),
            nn.ReLU(),
            nn.Linear(hidden_dim, 1),
            nn.Tanh()
        )

    def forward(self, state: torch.Tensor) -> torch.Tensor:
        return self.net(state)
```

---

## 9. DDPG 实现

### 7.1 经验回放

```python
# training/orca/replay_buffer.py

import numpy as np
from collections import deque
import random


class ReplayBuffer:
    """经验回放缓冲区"""

    def __init__(self, capacity: int = 100000):
        self.buffer = deque(maxlen=capacity)

    def push(self, state, action, reward, next_state, done):
        self.buffer.append((state, action, reward, next_state, done))

    def sample(self, batch_size: int):
        batch = random.sample(self.buffer, batch_size)

        states, actions, rewards, next_states, dones = zip(*batch)

        return (
            np.array(states, dtype=np.float32),
            np.array(actions, dtype=np.float32),
            np.array(rewards, dtype=np.float32).reshape(-1, 1),
            np.array(next_states, dtype=np.float32),
            np.array(dones, dtype=np.float32).reshape(-1, 1),
        )

    def __len__(self):
        return len(self.buffer)


class PrioritizedReplayBuffer:
    """优先经验回放（可选）"""

    def __init__(self, capacity: int = 100000, alpha: float = 0.6):
        self.capacity = capacity
        self.alpha = alpha
        self.buffer = []
        self.priorities = np.zeros(capacity, dtype=np.float32)
        self.pos = 0

    def push(self, state, action, reward, next_state, done):
        max_priority = self.priorities.max() if self.buffer else 1.0

        if len(self.buffer) < self.capacity:
            self.buffer.append((state, action, reward, next_state, done))
        else:
            self.buffer[self.pos] = (state, action, reward, next_state, done)

        self.priorities[self.pos] = max_priority
        self.pos = (self.pos + 1) % self.capacity

    def sample(self, batch_size: int, beta: float = 0.4):
        if len(self.buffer) == self.capacity:
            priorities = self.priorities
        else:
            priorities = self.priorities[:len(self.buffer)]

        probs = priorities ** self.alpha
        probs /= probs.sum()

        indices = np.random.choice(len(self.buffer), batch_size, p=probs)

        batch = [self.buffer[i] for i in indices]
        states, actions, rewards, next_states, dones = zip(*batch)

        # 重要性采样权重
        weights = (len(self.buffer) * probs[indices]) ** (-beta)
        weights /= weights.max()

        return (
            np.array(states, dtype=np.float32),
            np.array(actions, dtype=np.float32),
            np.array(rewards, dtype=np.float32).reshape(-1, 1),
            np.array(next_states, dtype=np.float32),
            np.array(dones, dtype=np.float32).reshape(-1, 1),
            indices,
            weights.astype(np.float32).reshape(-1, 1),
        )

    def update_priorities(self, indices, priorities):
        for idx, priority in zip(indices, priorities):
            self.priorities[idx] = priority + 1e-6

    def __len__(self):
        return len(self.buffer)
```

### 7.2 DDPG Agent

```python
# training/orca/ddpg.py

import torch
import torch.nn as nn
import torch.optim as optim
import numpy as np
from typing import Tuple

from network import Actor, Critic, OrcaActor
from replay_buffer import ReplayBuffer


class OUNoise:
    """Ornstein-Uhlenbeck 噪声（用于探索）"""

    def __init__(
        self,
        action_dim: int,
        mu: float = 0.0,
        theta: float = 0.15,
        sigma: float = 0.2
    ):
        self.action_dim = action_dim
        self.mu = mu
        self.theta = theta
        self.sigma = sigma
        self.state = np.ones(action_dim) * mu

    def reset(self):
        self.state = np.ones(self.action_dim) * self.mu

    def sample(self) -> np.ndarray:
        dx = self.theta * (self.mu - self.state) + self.sigma * np.random.randn(self.action_dim)
        self.state += dx
        return self.state


class DDPGAgent:
    """DDPG 智能体"""

    def __init__(
        self,
        state_dim: int = 7,
        action_dim: int = 1,
        hidden_dims: list = [256, 256],
        lr_actor: float = 1e-4,
        lr_critic: float = 1e-3,
        gamma: float = 0.99,
        tau: float = 0.005,
        buffer_size: int = 100000,
        batch_size: int = 256,
        device: str = 'cpu'
    ):
        self.device = torch.device(device)
        self.gamma = gamma
        self.tau = tau
        self.batch_size = batch_size

        # 创建网络
        self.actor = Actor(state_dim, hidden_dims, action_dim).to(self.device)
        self.actor_target = Actor(state_dim, hidden_dims, action_dim).to(self.device)
        self.actor_target.load_state_dict(self.actor.state_dict())

        self.critic = Critic(state_dim, action_dim, hidden_dims).to(self.device)
        self.critic_target = Critic(state_dim, action_dim, hidden_dims).to(self.device)
        self.critic_target.load_state_dict(self.critic.state_dict())

        # 优化器
        self.actor_optimizer = optim.Adam(self.actor.parameters(), lr=lr_actor)
        self.critic_optimizer = optim.Adam(self.critic.parameters(), lr=lr_critic)

        # 经验回放
        self.replay_buffer = ReplayBuffer(buffer_size)

        # 探索噪声
        self.noise = OUNoise(action_dim)

    def select_action(self, state: np.ndarray, add_noise: bool = True) -> np.ndarray:
        """选择动作"""
        state_tensor = torch.FloatTensor(state).unsqueeze(0).to(self.device)

        self.actor.eval()
        with torch.no_grad():
            action = self.actor(state_tensor).cpu().numpy()[0]
        self.actor.train()

        if add_noise:
            action += self.noise.sample()
            action = np.clip(action, -1, 1)

        return action

    def store_transition(self, state, action, reward, next_state, done):
        """存储经验"""
        self.replay_buffer.push(state, action, reward, next_state, done)

    def update(self) -> Tuple[float, float]:
        """更新网络"""
        if len(self.replay_buffer) < self.batch_size:
            return 0.0, 0.0

        # 采样
        states, actions, rewards, next_states, dones = self.replay_buffer.sample(self.batch_size)

        states = torch.FloatTensor(states).to(self.device)
        actions = torch.FloatTensor(actions).to(self.device)
        rewards = torch.FloatTensor(rewards).to(self.device)
        next_states = torch.FloatTensor(next_states).to(self.device)
        dones = torch.FloatTensor(dones).to(self.device)

        # ─── 更新 Critic ───
        with torch.no_grad():
            next_actions = self.actor_target(next_states)
            target_q = self.critic_target(next_states, next_actions)
            target_q = rewards + (1 - dones) * self.gamma * target_q

        current_q = self.critic(states, actions)
        critic_loss = nn.MSELoss()(current_q, target_q)

        self.critic_optimizer.zero_grad()
        critic_loss.backward()
        self.critic_optimizer.step()

        # ─── 更新 Actor ───
        actor_loss = -self.critic(states, self.actor(states)).mean()

        self.actor_optimizer.zero_grad()
        actor_loss.backward()
        self.actor_optimizer.step()

        # ─── 软更新 target 网络 ───
        self._soft_update(self.actor, self.actor_target)
        self._soft_update(self.critic, self.critic_target)

        return critic_loss.item(), actor_loss.item()

    def _soft_update(self, source: nn.Module, target: nn.Module):
        """软更新 target 网络"""
        for src_param, tgt_param in zip(source.parameters(), target.parameters()):
            tgt_param.data.copy_(self.tau * src_param.data + (1 - self.tau) * tgt_param.data)

    def save(self, path: str):
        """保存模型"""
        torch.save({
            'actor': self.actor.state_dict(),
            'actor_target': self.actor_target.state_dict(),
            'critic': self.critic.state_dict(),
            'critic_target': self.critic_target.state_dict(),
            'actor_optimizer': self.actor_optimizer.state_dict(),
            'critic_optimizer': self.critic_optimizer.state_dict(),
        }, path)

    def load(self, path: str):
        """加载模型"""
        checkpoint = torch.load(path, map_location=self.device)
        self.actor.load_state_dict(checkpoint['actor'])
        self.actor_target.load_state_dict(checkpoint['actor_target'])
        self.critic.load_state_dict(checkpoint['critic'])
        self.critic_target.load_state_dict(checkpoint['critic_target'])
        self.actor_optimizer.load_state_dict(checkpoint['actor_optimizer'])
        self.critic_optimizer.load_state_dict(checkpoint['critic_optimizer'])
```

---

## 10. 训练脚本

### 8.1 配置文件

```yaml
# training/orca/config.yaml

env:
  type: "simple"  # "simple" 或 "multi_flow"
  bandwidth_mbps: 100
  base_rtt_ms: 20
  buffer_size_packets: 100
  episode_steps: 200
  bandwidth_variation: true
  num_flows: 2  # 仅 multi_flow 使用

agent:
  state_dim: 7
  action_dim: 1
  hidden_dims: [256, 256]
  lr_actor: 0.0001
  lr_critic: 0.001
  gamma: 0.99
  tau: 0.005
  buffer_size: 100000
  batch_size: 256

training:
  num_episodes: 1000
  max_steps_per_episode: 200
  warmup_steps: 1000
  eval_interval: 50
  save_interval: 100
  noise_decay: 0.995
  min_noise: 0.1

output:
  model_dir: "checkpoints"
  onnx_path: "../../models/orca_actor.onnx"
```

### 8.2 训练脚本

```python
# training/orca/train.py

import os
import yaml
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import deque

from env import SimpleNetworkEnv, MultiFlowEnv
from ddpg import DDPGAgent


def load_config(path: str) -> dict:
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def create_env(config: dict):
    env_type = config['env']['type']

    if env_type == 'simple':
        return SimpleNetworkEnv(
            bandwidth_mbps=config['env']['bandwidth_mbps'],
            base_rtt_ms=config['env']['base_rtt_ms'],
            buffer_size_packets=config['env']['buffer_size_packets'],
            episode_steps=config['env']['episode_steps'],
            bandwidth_variation=config['env']['bandwidth_variation'],
        )
    elif env_type == 'multi_flow':
        return MultiFlowEnv(
            num_flows=config['env']['num_flows'],
            bandwidth_mbps=config['env']['bandwidth_mbps'],
            base_rtt_ms=config['env']['base_rtt_ms'],
            buffer_size_packets=config['env']['buffer_size_packets'],
            episode_steps=config['env']['episode_steps'],
        )
    else:
        raise ValueError(f"Unknown env type: {env_type}")


def evaluate(agent: DDPGAgent, env, num_episodes: int = 5) -> dict:
    """评估 agent"""
    total_rewards = []
    total_throughputs = []
    total_rtts = []
    total_losses = []

    for _ in range(num_episodes):
        state = env.reset()
        episode_reward = 0
        throughputs = []
        rtts = []
        losses = []

        done = False
        while not done:
            action = agent.select_action(state, add_noise=False)
            next_state, reward, done, info = env.step(action[0])

            episode_reward += reward
            throughputs.append(info['throughput'])
            rtts.append(info['rtt'])
            losses.append(info['loss_rate'])

            state = next_state

        total_rewards.append(episode_reward)
        total_throughputs.append(np.mean(throughputs))
        total_rtts.append(np.mean(rtts))
        total_losses.append(np.mean(losses))

    return {
        'reward': np.mean(total_rewards),
        'throughput': np.mean(total_throughputs),
        'rtt': np.mean(total_rtts) * 1000,  # ms
        'loss_rate': np.mean(total_losses),
    }


def plot_training(
    rewards: list,
    eval_metrics: list,
    save_path: str
):
    """绘制训练曲线"""
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))

    # Episode Rewards
    ax = axes[0, 0]
    ax.plot(rewards, alpha=0.3)
    # 移动平均
    window = min(50, len(rewards) // 10 + 1)
    if len(rewards) >= window:
        ma = np.convolve(rewards, np.ones(window)/window, mode='valid')
        ax.plot(range(window-1, len(rewards)), ma, 'r-', linewidth=2)
    ax.set_xlabel('Episode')
    ax.set_ylabel('Reward')
    ax.set_title('Training Rewards')
    ax.grid(True)

    if eval_metrics:
        episodes = [m['episode'] for m in eval_metrics]

        # Throughput
        ax = axes[0, 1]
        ax.plot(episodes, [m['throughput'] / 1e6 for m in eval_metrics], 'b-o')
        ax.set_xlabel('Episode')
        ax.set_ylabel('Throughput (MB/s)')
        ax.set_title('Evaluation Throughput')
        ax.grid(True)

        # RTT
        ax = axes[1, 0]
        ax.plot(episodes, [m['rtt'] for m in eval_metrics], 'g-o')
        ax.set_xlabel('Episode')
        ax.set_ylabel('RTT (ms)')
        ax.set_title('Evaluation RTT')
        ax.grid(True)

        # Loss Rate
        ax = axes[1, 1]
        ax.plot(episodes, [m['loss_rate'] * 100 for m in eval_metrics], 'r-o')
        ax.set_xlabel('Episode')
        ax.set_ylabel('Loss Rate (%)')
        ax.set_title('Evaluation Loss Rate')
        ax.grid(True)

    plt.tight_layout()
    plt.savefig(save_path)
    plt.close()


def main():
    parser = argparse.ArgumentParser(description='Train Orca DDPG')
    parser.add_argument('--config', type=str, default='config.yaml')
    args = parser.parse_args()

    # 加载配置
    config = load_config(args.config)

    # 创建输出目录
    os.makedirs(config['output']['model_dir'], exist_ok=True)

    # 创建环境和 agent
    env = create_env(config)
    agent = DDPGAgent(
        state_dim=config['agent']['state_dim'],
        action_dim=config['agent']['action_dim'],
        hidden_dims=config['agent']['hidden_dims'],
        lr_actor=config['agent']['lr_actor'],
        lr_critic=config['agent']['lr_critic'],
        gamma=config['agent']['gamma'],
        tau=config['agent']['tau'],
        buffer_size=config['agent']['buffer_size'],
        batch_size=config['agent']['batch_size'],
    )

    print(f"Environment: {config['env']['type']}")
    print(f"State dim: {env.state_dim}, Action dim: {env.action_dim}")
    print(f"Training for {config['training']['num_episodes']} episodes")

    # 训练
    episode_rewards = []
    eval_metrics = []
    noise_scale = 1.0

    for episode in range(config['training']['num_episodes']):
        state = env.reset()
        agent.noise.reset()
        episode_reward = 0

        for step in range(config['training']['max_steps_per_episode']):
            # 选择动作
            action = agent.select_action(state, add_noise=True)
            action = action * noise_scale  # 噪声衰减
            action = np.clip(action, -1, 1)

            # 执行动作
            next_state, reward, done, info = env.step(action[0])

            # 存储经验
            agent.store_transition(state, action, reward, next_state, done)

            # 更新网络
            if len(agent.replay_buffer) >= config['training']['warmup_steps']:
                agent.update()

            state = next_state
            episode_reward += reward

            if done:
                break

        episode_rewards.append(episode_reward)

        # 噪声衰减
        noise_scale = max(
            config['training']['min_noise'],
            noise_scale * config['training']['noise_decay']
        )

        # 打印进度
        if (episode + 1) % 10 == 0:
            avg_reward = np.mean(episode_rewards[-10:])
            print(f"Episode {episode + 1:4d} | Reward: {episode_reward:7.2f} | "
                  f"Avg(10): {avg_reward:7.2f} | Noise: {noise_scale:.3f}")

        # 评估
        if (episode + 1) % config['training']['eval_interval'] == 0:
            metrics = evaluate(agent, env)
            metrics['episode'] = episode + 1
            eval_metrics.append(metrics)
            print(f"  [Eval] Throughput: {metrics['throughput']/1e6:.2f} MB/s | "
                  f"RTT: {metrics['rtt']:.1f} ms | Loss: {metrics['loss_rate']*100:.2f}%")

        # 保存
        if (episode + 1) % config['training']['save_interval'] == 0:
            agent.save(os.path.join(config['output']['model_dir'], f'checkpoint_{episode+1}.pth'))

    # 保存最终模型
    agent.save(os.path.join(config['output']['model_dir'], 'final_model.pth'))

    # 绘制训练曲线
    plot_training(
        episode_rewards,
        eval_metrics,
        os.path.join(config['output']['model_dir'], 'training_curves.png')
    )

    print("\nTraining complete!")
    print(f"Final evaluation: {evaluate(agent, env)}")


if __name__ == '__main__':
    main()
```

---

## 11. 导出 ONNX

```python
# training/orca/export_onnx.py

import os
import yaml
import argparse
import torch
import onnx
import onnxruntime as ort
import numpy as np

from network import Actor, OrcaActor


def load_config(path: str) -> dict:
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def export_onnx(model, state_dim: int, output_path: str):
    """导出 Actor 到 ONNX"""
    model.eval()

    # 示例输入
    dummy_input = torch.randn(1, state_dim)

    # 导出
    torch.onnx.export(
        model,
        dummy_input,
        output_path,
        input_names=['state'],
        output_names=['action'],
        dynamic_axes={
            'state': {0: 'batch_size'},
            'action': {0: 'batch_size'}
        },
        opset_version=17,
        do_constant_folding=True
    )

    print(f"Exported to {output_path}")

    # 验证
    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)
    print("ONNX model validation passed!")


def verify_onnx(pytorch_model, onnx_path: str, state_dim: int):
    """验证 ONNX 与 PyTorch 输出一致"""
    pytorch_model.eval()

    # 测试输入
    test_input = np.random.randn(5, state_dim).astype(np.float32)

    # PyTorch
    with torch.no_grad():
        pytorch_output = pytorch_model(torch.from_numpy(test_input)).numpy()

    # ONNX
    session = ort.InferenceSession(onnx_path)
    onnx_output = session.run(None, {'state': test_input})[0]

    max_diff = np.abs(pytorch_output - onnx_output).max()
    print(f"Max difference: {max_diff:.8f}")

    if max_diff < 1e-5:
        print("Verification PASSED!")
    else:
        print("WARNING: Outputs differ!")


def create_lightweight_model(checkpoint_path: str, config: dict) -> OrcaActor:
    """从完整 checkpoint 创建轻量级 Actor"""
    # 加载完整模型
    checkpoint = torch.load(checkpoint_path, map_location='cpu')

    full_actor = Actor(
        state_dim=config['agent']['state_dim'],
        hidden_dims=config['agent']['hidden_dims'],
        action_dim=config['agent']['action_dim']
    )
    full_actor.load_state_dict(checkpoint['actor'])
    full_actor.eval()

    # 创建轻量级模型并知识蒸馏（简化版：直接用小模型重新训练）
    # 这里简单返回完整模型，实际部署可以训练一个小模型
    return full_actor


def main():
    parser = argparse.ArgumentParser(description='Export Orca model to ONNX')
    parser.add_argument('--config', type=str, default='config.yaml')
    parser.add_argument('--checkpoint', type=str, default='checkpoints/final_model.pth')
    parser.add_argument('--lightweight', action='store_true', help='Export lightweight model')
    args = parser.parse_args()

    config = load_config(args.config)

    # 加载模型
    checkpoint = torch.load(args.checkpoint, map_location='cpu')

    if args.lightweight:
        model = OrcaActor(
            state_dim=config['agent']['state_dim'],
            hidden_dim=64
        )
        # 注意：轻量级模型需要单独训练或蒸馏
        print("Warning: Lightweight model needs separate training")
    else:
        model = Actor(
            state_dim=config['agent']['state_dim'],
            hidden_dims=config['agent']['hidden_dims'],
            action_dim=config['agent']['action_dim']
        )
        model.load_state_dict(checkpoint['actor'])

    model.eval()
    print(f"Loaded model from {args.checkpoint}")
    print(f"Parameters: {sum(p.numel() for p in model.parameters()):,}")

    # 导出
    output_path = config['output']['onnx_path']
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    export_onnx(model, config['agent']['state_dim'], output_path)
    verify_onnx(model, output_path, config['agent']['state_dim'])

    # 打印模型信息
    print("\n" + "=" * 50)
    print("ONNX Model Info")
    print("=" * 50)
    print(f"File size: {os.path.getsize(output_path) / 1024:.2f} KB")

    onnx_model = onnx.load(output_path)
    print(f"Input: {onnx_model.graph.input[0].name}")
    print(f"Output: {onnx_model.graph.output[0].name}")


if __name__ == '__main__':
    main()
```

---

## 12. 运行训练

```bash
# 1. 创建目录
mkdir -p training/orca

# 2. 复制代码文件到 training/orca/
# env.py, network.py, ddpg.py, replay_buffer.py, train.py, export_onnx.py, config.yaml

# 3. 训练
cd training/orca
python train.py --config config.yaml

# 4. 导出 ONNX
python export_onnx.py --config config.yaml
```

预期输出：
```
Environment: simple
State dim: 7, Action dim: 1
Training for 1000 episodes
Episode   10 | Reward:  -45.32 | Avg(10):  -52.18 | Noise: 0.951
Episode   20 | Reward:  -28.45 | Avg(10):  -35.67 | Noise: 0.904
...
Episode  500 | Reward:   12.34 | Avg(10):   10.56 | Noise: 0.182
  [Eval] Throughput: 8.45 MB/s | RTT: 25.3 ms | Loss: 0.52%
...
Episode 1000 | Reward:   18.76 | Avg(10):   17.23 | Noise: 0.100
  [Eval] Throughput: 10.12 MB/s | RTT: 22.1 ms | Loss: 0.15%

Training complete!
```

---

## 13. C++ 集成

> **详细的架构说明请参考 Part 1 的 4.2-4.4 节。**

### 13.1 Orca 模型输入

模型是 7 维输入：

```cpp
// include/neustack/ai/ai_model.hpp 中的 OrcaModel::Input

struct Input {
    float throughput_normalized;      // 吞吐量 / MAX_THROUGHPUT
    float queuing_delay_normalized;   // (RTT - min_RTT) / min_RTT
    float rtt_ratio;                  // RTT / min_RTT
    float loss_rate;                  // 丢包率 [0, 1]
    float cwnd_normalized;            // cwnd / MAX_CWND
    float in_flight_ratio;            // bytes_in_flight / (cwnd × MSS)
    float predicted_bw_normalized;    // 带宽预测 / MAX_THROUGHPUT (第7维)
};
```

### 13.2 启用 Orca 拥塞控制

**步骤 1：修改 TCPConnectionManager 使用 Orca**

```cpp
// src/transport/tcp_connection.cpp 中的 create_tcb()

#include "neustack/transport/tcp_orca.hpp"

TCB *TCPConnectionManager::create_tcb(const TCPTuple &t_tuple) {
    // ... 省略前面代码 ...

    // 使用 Orca 而非 Reno
    tcb->congestion_control = std::make_unique<TCPOrca>(tcb->options.mss);

    // ... 省略后面代码 ...
}
```

**步骤 2：实现 apply_cwnd_action()**

```cpp
// include/neustack/transport/tcp_layer.hpp 中

#include "neustack/transport/tcp_orca.hpp"

void apply_cwnd_action(const AIAction& action) {
    for (auto& [tuple, tcb] : _tcp_mgr._connections) {
        auto* orca = dynamic_cast<TCPOrca*>(tcb->congestion_control.get());
        if (orca) {
            orca->set_alpha(action.cwnd.alpha);
        }
    }
}
```

**步骤 3：启用 ai_test 中的 Orca 模型**

重新训练模型后，恢复 ai_test 中的 Orca：

```cpp
// examples/ai_test.cpp

IntelligencePlaneConfig config;
config.orca_model_path = "../models/orca_actor.onnx";  // 取消注释
config.anomaly_model_path = "../models/anomaly_detector.onnx";
config.bandwidth_model_path = "../models/bandwidth_predictor.onnx";

tcp.enable_ai(config);
```

---

## 14. 与 CUBIC 对比

### 12.1 Baseline 对比实验

```python
# training/orca/compare.py

import numpy as np
import matplotlib.pyplot as plt
from env import SimpleNetworkEnv
from ddpg import DDPGAgent


def run_cubic_only(env, num_episodes=10):
    """纯 CUBIC（α=0）"""
    results = []

    for _ in range(num_episodes):
        state = env.reset()
        metrics = {'throughput': [], 'rtt': [], 'loss': []}

        done = False
        while not done:
            # α=0 相当于纯 CUBIC
            next_state, reward, done, info = env.step(0.0)
            metrics['throughput'].append(info['throughput'])
            metrics['rtt'].append(info['rtt'])
            metrics['loss'].append(info['loss_rate'])
            state = next_state

        results.append({
            'throughput': np.mean(metrics['throughput']),
            'rtt': np.mean(metrics['rtt']),
            'loss': np.mean(metrics['loss']),
        })

    return results


def run_orca(env, agent, num_episodes=10):
    """Orca (DDPG)"""
    results = []

    for _ in range(num_episodes):
        state = env.reset()
        metrics = {'throughput': [], 'rtt': [], 'loss': []}

        done = False
        while not done:
            action = agent.select_action(state, add_noise=False)
            next_state, reward, done, info = env.step(action[0])
            metrics['throughput'].append(info['throughput'])
            metrics['rtt'].append(info['rtt'])
            metrics['loss'].append(info['loss_rate'])
            state = next_state

        results.append({
            'throughput': np.mean(metrics['throughput']),
            'rtt': np.mean(metrics['rtt']),
            'loss': np.mean(metrics['loss']),
        })

    return results


def compare_and_plot():
    env = SimpleNetworkEnv(bandwidth_variation=True)

    # 加载训练好的 Orca
    agent = DDPGAgent()
    agent.load('checkpoints/final_model.pth')

    # 运行对比
    cubic_results = run_cubic_only(env, num_episodes=20)
    orca_results = run_orca(env, agent, num_episodes=20)

    # 打印结果
    print("=" * 50)
    print("Comparison Results")
    print("=" * 50)
    print(f"{'Metric':<15} {'CUBIC':<15} {'Orca':<15} {'Improvement':<15}")
    print("-" * 60)

    cubic_throughput = np.mean([r['throughput'] for r in cubic_results]) / 1e6
    orca_throughput = np.mean([r['throughput'] for r in orca_results]) / 1e6
    print(f"{'Throughput':<15} {cubic_throughput:>10.2f} MB/s {orca_throughput:>10.2f} MB/s "
          f"{(orca_throughput/cubic_throughput - 1)*100:>+10.1f}%")

    cubic_rtt = np.mean([r['rtt'] for r in cubic_results]) * 1000
    orca_rtt = np.mean([r['rtt'] for r in orca_results]) * 1000
    print(f"{'RTT':<15} {cubic_rtt:>10.2f} ms   {orca_rtt:>10.2f} ms   "
          f"{(1 - orca_rtt/cubic_rtt)*100:>+10.1f}%")

    cubic_loss = np.mean([r['loss'] for r in cubic_results]) * 100
    orca_loss = np.mean([r['loss'] for r in orca_results]) * 100
    print(f"{'Loss Rate':<15} {cubic_loss:>10.2f}%     {orca_loss:>10.2f}%     "
          f"{(1 - orca_loss/cubic_loss)*100:>+10.1f}%")


if __name__ == '__main__':
    compare_and_plot()
```

预期输出：
```
==================================================
Comparison Results
==================================================
Metric          CUBIC           Orca            Improvement
------------------------------------------------------------
Throughput           8.23 MB/s      10.12 MB/s       +23.0%
RTT                 28.50 ms        22.10 ms        +22.5%
Loss Rate            1.20%          0.15%           +87.5%
```

---

## 15. 新增文件清单

```
training/orca/
├── config.yaml
├── env.py              # 网络模拟环境
├── network.py          # Actor/Critic 网络
├── ddpg.py             # DDPG Agent
├── replay_buffer.py    # 经验回放
├── train.py            # 训练脚本
├── export_onnx.py      # 导出 ONNX
└── compare.py          # 与 CUBIC 对比

models/
└── orca_actor.onnx     # 训练后的模型
```

---

## 16. 常见问题

### Q1: 训练不收敛？

- 增加 warmup_steps
- 降低学习率
- 检查奖励函数设计

### Q2: 推理延迟太高？

- 使用 OrcaActor 轻量版
- 降低推理频率（每 10ms 而非每 ACK）

### Q3: 泛化性差？

- 在多种网络条件下训练
- 使用 domain randomization
- 增加训练 episode 数

---

## 17. 下一步

- **教程 07: 端到端集成测试** — 三模型协同工作
- **教程 08: 性能评估** — 真实网络测试

---

## 18. 参考资料

- [Orca: NSDI 2022](https://www.usenix.org/conference/nsdi22/presentation/abbasloo)
- [DDPG Paper](https://arxiv.org/abs/1509.02971)
- [CUBIC RFC 8312](https://tools.ietf.org/html/rfc8312)
- [PyTorch RL Tutorial](https://pytorch.org/tutorials/intermediate/reinforcement_q_learning.html)
