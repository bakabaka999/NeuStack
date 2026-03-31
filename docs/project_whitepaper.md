# NeuStack 项目白皮书

> **Version:** 1.5
> **Last Updated:** 2026-03-31
> **Status:** Active

---

## 1. 项目概述

### 1.1 项目定位

**NeuStack** 是一个**完全从零实现**的跨平台用户态 TCP/IP 协议栈，集成 AI 拥塞控制智能面与 AF_XDP 高性能数据路径。C++ 实现协议栈核心（约 10,000 行），Python 实现 AI 模型训练管线（约 4,000 行），Shell/Python 脚本实现数据采集、基准测试与环境配置（约 2,700 行）。

区别于传统内核协议栈的黑盒实现，NeuStack 通过以下核心特性实现差异化：

| 特性 | 描述 |
|------|------|
| **跨平台 HAL** | 硬件抽象层屏蔽 macOS (utun) / Linux (TUN/TAP + AF_XDP) / Windows (Wintun) 底层差异 |
| **AF_XDP 数据路径** | Linux 高性能 backend：UMEM 共享内存环、BPF/XDP 程序加载、批量收发，generic copy mode 下 1.45× 内核 UDP |
| **AI 智能面** | 三模型协同决策：Orca (SAC) 拥塞控制 + LSTM 带宽预测 + Autoencoder 异常检测 |
| **NetworkAgent** | 4 状态决策层，协调 3 个 AI 模型，实现策略性 clamp / 回退 / 连接控制 |
| **零分配热路径** | FixedPool slab 分配器，包处理循环中无 new/delete |
| **Telemetry** | MetricsRegistry + 7 个 HTTP 端点 + Prometheus 导出 + neustack-stat 实时 CLI |
| **双线程架构** | 数据面 + AI 推理面分离，通过无锁 SPSC 队列异步通信 |

### 1.2 解决的问题

1. **内核协议栈的不透明性**：无法细粒度控制 TCP 行为，难以植入 AI 模型
2. **传统 CC 算法的局限性**：Reno/CUBIC 等固定数学模型难以适应复杂动态网络环境
3. **跨平台开发的碎片化**：不同操作系统的 TUN/TAP 接口差异巨大
4. **AI 与网络的割裂**：现有研究往往在模拟器中训练，难以部署到真实协议栈

### 1.3 目标用户

- 网络协议研究人员
- AI/ML 与网络交叉领域的研究者
- 需要定制网络行为的应用开发者

---

## 2. 系统架构

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application Layer                          │
│             HTTP Server/Client  ·  DNS Client                   │
│                   ┌─────────────────────┐                       │
│                   │    NeuStack API     │                       │
│                   │  (Unified Facade)   │                       │
│                   └──────────┬──────────┘                       │
├──────────────────────────────┼──────────────────────────────────┤
│                      Transport Layer                            │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    TCP Engine                             │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐   │   │
│  │  │ State       │  │ Sliding     │  │ Retransmission  │   │   │
│  │  │ Machine     │  │ Window      │  │ & Timers        │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────┘   │   │
│  │                          │                                │   │
│  │  ┌──────────────────────────────────────────────────────┐ │   │
│  │  │          Congestion Control (Pluggable)              │ │   │
│  │  │   Reno (RFC)  ·  CUBIC (RFC 8312)  ·  Orca (SAC)    │ │   │
│  │  └──────────────────────────────────────────────────────┘ │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    UDP                                    │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│                       Network Layer                             │
│             ┌──────────────┐  ┌──────────────┐                  │
│             │     IPv4     │  │     ICMP     │                  │
│             │   Routing    │  │  Echo/Reply  │                  │
│             └──────────────┘  └──────────────┘                  │
├─────────────────────────────────────────────────────────────────┤
│                 Hardware Abstraction Layer                       │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                   NetDevice (Abstract)                    │   │
│  ├──────────────┬──────────────────┬───────────────────────┤   │
│  │ macOS: utun  │  Linux: TUN/TAP  │  Windows: Wintun      │   │
│  └──────────────┴──────────────────┴───────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
        ↕                                        ↕
┌────────────────┐    ┌──────────────────────────────────┐
│    指标采集     │    │        NetworkAgent (决策层)       │
│  TCP Sample    │───→│  ┌────────┬──────────┬─────────┐ │
│  Global Metrics│    │  │ Orca   │ 带宽预测  │ 异常检测 │ │
│  CSV Export    │    │  │ (SAC)  │ (LSTM)   │  (AE)   │ │
└────────────────┘    │  └────────┴──────────┴─────────┘ │
                      │  状态机: NORMAL → DEGRADED →      │
                      │         UNDER_ATTACK → RECOVERY   │
                      └──────────────────────────────────┘
                                       ↑
                      ┌──────────────────────────────────┐
                      │        训练管线 (Python)           │
                      │  PyTorch 训练 → ONNX 导出          │
                      │  数据采集 → 预处理 → 离线训练       │
                      └──────────────────────────────────┘
```

### 2.2 模块职责详述

#### 2.2.1 NeuStack Facade (`include/neustack/neustack.hpp`)

提供统一的应用层入口，封装整个协议栈的生命周期和组件访问。

```cpp
struct StackConfig {
    std::string local_ip = "192.168.100.2";
    uint32_t dns_server = 0x08080808;
    LogLevel log_level = LogLevel::INFO;
    bool enable_icmp = true;
    bool enable_udp = true;
    std::string device_type = "tun";
    std::string device_ifname;
    int io_cpu = -1;
    bool enable_firewall = true;
    bool firewall_shadow_mode = true;
    std::string orca_model_path;
    std::string anomaly_model_path;
    std::string bandwidth_model_path;
    std::string security_model_path;
    float security_threshold = 0.0f;
    std::string data_output_dir;
    int security_label = 0;
};

class NeuStack {
public:
    static std::unique_ptr<NeuStack> create(const StackConfig& config = {});

    // 关键组件访问
    NetDevice& device();
    IPv4Layer& ip();
    ICMPHandler* icmp();
    UDPLayer* udp();
    HttpServer& http_server();
    HttpClient& http_client();
    DNSClient*  dns();
    TCPLayer&   tcp();
    telemetry::TelemetryAPI& telemetry();

    // 生命周期 / 观测
    std::string status_json(bool pretty = false);
    std::string status_prometheus();
    bool ai_enabled() const;
    void run();   // 阻塞运行（Ctrl+C 退出）
    void stop();
};
```

#### 2.2.2 TCP Engine (`src/transport/`)

TCP 核心实现，遵循 RFC 9293/5681/6298/8312 规范。

| 子模块 | 头文件 | 实现文件 | 职责 |
|--------|--------|----------|------|
| 连接层 | `tcp_layer.hpp` | `tcp_layer.cpp` | TCP 层协调器，连接管理，AI 集成 |
| TCB | `tcp_tcb.hpp` | `tcp_connection.cpp` | TCP 控制块，连接状态机，重传队列 |
| 段解析 | `tcp_segment.hpp` | `tcp_segment.cpp` | TCP 报文解析与头部操作 |
| 段构造 | `tcp_builder.hpp` | `tcp_builder.cpp` | 从零构造 TCP 报文 |
| 状态机 | `tcp_state.hpp` | — | TCP FSM 状态定义与转换 |
| 序列号 | `tcp_seq.hpp` | — | ISN 生成、序列号比较与环绕处理 |
| 流接口 | `stream.hpp` | — | 面向应用层的流式读写抽象 |

**TCP 状态机转换图：**

```
                              ┌──────────────┐
                              │    CLOSED    │
                              └──────┬───────┘
                    ┌────────────────┼────────────────┐
                    │ passive open   │                │ active open
                    ▼                │                ▼
             ┌──────────┐           │         ┌──────────────┐
             │  LISTEN  │           │         │   SYN_SENT   │
             └────┬─────┘           │         └──────┬───────┘
          rcv SYN │                 │                │ rcv SYN+ACK
       send SYN+ACK                │                │ send ACK
                  ▼                 │                ▼
             ┌──────────────┐      │         ┌──────────────┐
             │ SYN_RECEIVED │──────┼────────▶│ ESTABLISHED  │
             └──────────────┘  ACK │         └──────┬───────┘
                                   │         close  │  rcv FIN
                              ┌────┼────┐    ┌──────┴───────┐
                              │    │    │    │              │
                              ▼    │    ▼    ▼              ▼
                       ┌───────────┐  ┌──────────┐  ┌──────────┐
                       │FIN_WAIT_1 │  │FIN_WAIT_2│  │CLOSE_WAIT│
                       └─────┬─────┘  └────┬─────┘  └────┬─────┘
                             │             │              │
                             ▼             ▼              ▼
                       ┌───────────┐  ┌──────────┐  ┌──────────┐
                       │  CLOSING  │  │TIME_WAIT │  │ LAST_ACK │
                       └───────────┘  └──────────┘  └──────────┘
```

#### 2.2.3 拥塞控制模块 (`include/neustack/transport/`)

通过 `ICongestionControl` 接口实现可插拔的拥塞控制算法：

```cpp
class ICongestionControl {
public:
    virtual void on_ack(uint32_t bytes_acked, uint32_t rtt_us) = 0;
    virtual void on_loss(uint32_t bytes_lost) = 0;
    virtual uint32_t cwnd() const = 0;
    virtual uint32_t ssthresh() const = 0;
};
```

**三种算法实现：**

| 算法 | 文件 | 特性 |
|------|------|------|
| **TCP Reno** | `tcp_reno.hpp` | 标准 AIMD：慢启动、拥塞避免、快速重传与快速恢复 |
| **CUBIC** | `tcp_cubic.hpp` | RFC 8312 实现：三次函数窗口增长，TCP 友好模式，BDP 快速恢复 |
| **Orca** | `tcp_orca.hpp` | NSDI 2022：CUBIC 基础上叠加 AI 调制，`cwnd = 2^α × cwnd_cubic` |

**Orca 双层架构：**

```
Layer 1: CUBIC 计算 cwnd_cubic（RFC 8312 标准流程）
                    │
                    ▼
Layer 2: AI Actor 输出 α ∈ [-1, 1]
                    │
                    ▼
         最终 cwnd = 2^α × cwnd_cubic
         （α > 0 放大窗口，α < 0 收缩窗口）
```

#### 2.2.4 AI 智能面 (`include/neustack/ai/`, `src/ai/`)

AI 子系统采用双线程架构，将推理计算从数据面分离：

| 模块 | 头文件 | 职责 |
|------|--------|------|
| ONNX 推理引擎 | `onnx_inference.hpp` | ONNX Runtime C++ API 封装 |
| Orca 模型 | `orca_model.hpp` | SAC Actor 网络推理 |
| 带宽预测模型 | `bandwidth_model.hpp` | LSTM 带宽预测推理 |
| 异常检测模型 | `anomaly_model.hpp` | LSTM-Autoencoder 异常检测推理 |
| NetworkAgent | `ai_agent.hpp` | 4 状态决策层，协调三模型输出 |
| IntelligencePlane | `intelligence_plane.hpp` | 双线程架构：AI 推理线程 + 数据面异步通信 |

**双线程通信架构：**

```
数据面（主线程）                      AI 推理面（独立线程）
┌─────────────┐                    ┌─────────────────────┐
│ TCP 收发包   │                    │ ONNX Runtime 推理    │
│ 状态机更新   │                    │                     │
│ 指标采集     │                    │ Orca:     每 10ms   │
│  TCPSample  │──MetricsBuffer──→ │ 带宽预测:  每 100ms  │
│  GlobalMetrics│                   │ 异常检测:  每 1s     │
│             │                    │                     │
│ 应用 AI 动作 │←─SPSCQueue<AIAction>─│ NetworkAgent 决策   │
│ 调节 α 值   │                    │ 输出 AIAction       │
└─────────────┘                    └─────────────────────┘
```

#### 2.2.5 指标采集 (`include/neustack/metrics/`)

| 模块 | 头文件 | 职责 |
|------|--------|------|
| 全局指标 | `global_metrics.hpp` | 原子计数器：收发包数、字节数、TCP 标志、连接数 |
| TCP 采样 | `tcp_sample.hpp` | 每连接采样（48 字节）：时间戳、RTT、cwnd、吞吐量、丢包率 |
| 特征提取 | `ai_features.hpp` | 将原始指标归一化为 AI 模型所需的特征向量 |
| AI 动作 | `ai_action.hpp` | AI 决策结构体：CWND_ADJUST / ANOMALY_ALERT / BW_PREDICTION |
| CSV 导出 | `sample_exporter.hpp`, `metric_exporter.hpp` | 原始采样与聚合指标的 CSV 导出 |

#### 2.2.6 Network Layer (`src/net/`)

| 模块 | 头文件 | 实现文件 | 核心功能 |
|------|--------|----------|----------|
| IPv4 | `ipv4.hpp` | `ipv4.cpp` | 报文解析、校验和计算、协议分发（ICMP/TCP/UDP） |
| ICMP | `icmp.hpp` | `icmp.cpp` | Echo Request/Reply、Destination Unreachable |

**IPv4 报文处理流程：**

```
收包: NetDevice::recv()
        │
        ▼
  ┌───────────────┐
  │ 校验和验证     │
  └───────┬───────┘
          │
          ▼
  ┌───────────────┐
  │ 协议分发      │
  └───────┬───────┘
          │
    ┌─────┴─────┬──────────────┐
    ▼           ▼              ▼
┌───────┐  ┌────────┐    ┌──────────┐
│ ICMP  │  │  TCP   │    │   UDP    │
└───────┘  └────────┘    └──────────┘
```

#### 2.2.7 Application Layer (`include/neustack/app/`)

| 模块 | 头文件 | 功能 |
|------|--------|------|
| HTTP 服务器 | `http_server.hpp` | 路由注册、请求分发、响应生成、分块传输编码、流式响应 |
| HTTP 客户端 | `http_client.hpp` | 请求构造、响应解析 |
| HTTP 解析器 | `http_parser.hpp` | HTTP/1.1 请求解析（方法、路径、头部、请求体） |
| HTTP 类型 | `http_types.hpp` | HttpRequest/HttpResponse 结构体、Content-Type 常量 |
| DNS 客户端 | `dns_client.hpp` | 基于 UDP 的域名解析 |

#### 2.2.8 Hardware Abstraction Layer (`src/hal/`)

```cpp
class NetDevice {
public:
    virtual ~NetDevice() = default;
    virtual int open() = 0;
    virtual int close() = 0;
    virtual ssize_t send(const uint8_t* data, size_t len) = 0;
    virtual ssize_t recv(uint8_t* buf, size_t len, int timeout_ms = -1) = 0;
    virtual int get_fd() const = 0;
    virtual std::string get_name() const = 0;
    static std::unique_ptr<NetDevice> create();
};
```

**平台特定实现：**

| 平台 | 文件 | 底层技术 | 关键系统调用 |
|------|------|----------|--------------|
| macOS | `hal_macos.cpp` | utun (L3 点对点) | `socket(PF_SYSTEM, ...)`, `ioctl(CTLIOCGINFO)` |
| Linux (TUN) | `hal_linux.cpp` | TUN/TAP (L2/L3) | `open("/dev/net/tun")`, `ioctl(TUNSETIFF)` |
| Linux (AF_XDP) | `hal_linux_afxdp.cpp` | AF_XDP + UMEM + BPF | `socket(AF_XDP, ...)`, `mmap()`, `sendto()` |
| Windows | `hal_windows.cpp` | Wintun | `WintunCreateAdapter()`, Ring Buffer API |

#### 2.2.9 通用基础设施 (`include/neustack/common/`)

| 模块 | 头文件 | 功能 |
|------|--------|------|
| 校验和 | `checksum.hpp` | RFC 1071 Internet 校验和，accumulate/finalize 模式 |
| IP 地址 | `ip_addr.hpp` | IP 地址解析与工具函数 |
| ISN 生成 | `isn_generator.hpp` | TCP 初始序列号安全随机生成 |
| 环形缓冲区 | `ring_buffer.hpp` | StreamBuffer：TCP 发送/接收缓冲区（64KB，零拷贝 head consume） |
| SPSC 队列 | `spsc_queue.hpp` | 无锁单生产者单消费者队列（power-of-2，trivially-copyable） |
| 内存池 | `fixed_pool.hpp` | FixedPool slab 分配器，热路径零 new/delete |
| 日志 | `log.hpp` | 分级日志系统（TRACE/DEBUG/INFO/WARN/ERROR） |

---

## 3. AI 智能面详述

### 3.1 三个 AI 模型

| 模型 | 算法 | 输入 | 输出 | 推理频率 | 用途 |
|------|------|------|------|----------|------|
| **Orca** | SAC (Soft Actor-Critic) | 7 维网络状态 | α ∈ [-1, 1] | 10ms | CWND 调制系数 |
| **带宽预测** | LSTM (2 层) | 30 步 × 3 维时序 | 归一化带宽 [0, 1] | 100ms | 前瞻性带宽估计 |
| **异常检测** | LSTM-Autoencoder | 8 维网络特征 | 重构误差 (MSE) | 1s | 检测网络异常/攻击 |

#### 3.1.1 Orca Actor 网络

**输入特征（7 维，归一化）：**

| 特征 | 归一化方式 | 描述 |
|------|-----------|------|
| `throughput_normalized` | delivery_rate / est_bw, clip [0, 2] | 归一化吞吐量 |
| `queuing_delay_normalized` | (rtt − min_rtt) / min_rtt, clip [0, 5] | 排队延迟 |
| `rtt_ratio` | rtt / min_rtt, clip [1, 5] | RTT 膨胀比 |
| `loss_rate` | packets_lost / packets_sent, clip [0, 1] | 丢包率 |
| `cwnd_normalized` | cwnd / BDP, clip [0, 10] | 窗口与 BDP 之比 |
| `in_flight_ratio` | bytes_in_flight / (cwnd × MSS), clip [0, 2] | 在途数据填充率 |
| `predicted_bw_normalized` | predicted_bw / 10MB/s, clip [0, 2] | LSTM 预测带宽 |

**输出动作：**

| 动作 | 取值范围 | 描述 |
|------|----------|------|
| α | [-1, 1] | CWND 调制系数，最终 `cwnd = 2^α × cwnd_cubic` |

**模型规格：**

```yaml
Framework: PyTorch → ONNX
Algorithm: SAC (Soft Actor-Critic)
Input Shape: [1, 7]
Output Shape: [1, 1]  (tanh 激活，输出 [-1, 1])
Inference Engine: ONNX Runtime (C++ API)
Model File: models/orca_actor.onnx
```

#### 3.1.2 LSTM 带宽预测器

```yaml
Architecture: 2-layer LSTM + sigmoid output
Input Shape: [1, 30, 3]  (30 timesteps × 3 features: throughput, rtt_ratio, loss)
Output Shape: [1, 1]  (归一化预测带宽 [0, 1])
Hidden Dim: 64
Denormalization: predicted_bw = output × 10 MB/s
Model File: models/bandwidth_predictor.onnx
```

#### 3.1.3 LSTM-Autoencoder 异常检测器

**输入特征（8 维，归一化）：**

| 特征 | 归一化因子 | 描述 |
|------|-----------|------|
| `packets_rx_norm` | / 20000 | 接收包速率 |
| `packets_tx_norm` | / 20000 | 发送包速率 |
| `bytes_tx_norm` | / 30000000 | 发送字节速率 |
| `syn_rate_norm` | / 100 | SYN 包速率 |
| `rst_rate_norm` | / 100 | RST 包速率 |
| `conn_established_norm` | / 100 | 建立连接数 |
| `tx_rx_ratio_norm` | (tx/rx) / 10 | 收发比 |
| `active_conn_norm` | / 100 | 活跃连接数 |

```yaml
Architecture: LSTM encoder → latent bottleneck → LSTM decoder
Input Shape: [1, 8]
Output Shape: [1, 8]  (重构向量)
Anomaly Score: MSE(input, output)
Threshold: 0.5 (可配置)
Model File: models/anomaly_detector.onnx
```

### 3.2 NetworkAgent 决策层

Agent 是一个 4 状态有限状态机，融合三个模型的输出，做出策略性决策：

```
                 带宽骤降 >50%
    ┌─────────┐ ──────────────→ ┌───────────┐
    │ NORMAL  │                 │ DEGRADED  │
    │ α∈[-1,1]│ ←────────────── │ α∈[-1,0.3]│
    └────┬────┘  带宽恢复 >85%  └───────────┘
         │                            │
         │ 异常检测触发                │ 异常检测触发
         ↓                            ↓
    ┌──────────────┐           ┌──────────────┐
    │ UNDER_ATTACK │ ────────→ │  RECOVERY    │
    │ α=0 (回退    │ 连续50次  │ α∈[-1,0.5]  │
    │   CUBIC)     │  正常     │ 100 tick后   │──→ NORMAL
    └──────────────┘           └──────────────┘
```

**每个状态下 Agent 的行为不同：**

| 状态 | Orca α 范围 | 新连接 | 触发条件 |
|------|-------------|--------|----------|
| **NORMAL** | [-1, 1]（完全信任 Orca） | 允许 | 默认状态 |
| **DEGRADED** | [-1, 0.3]（限制激进增窗） | 允许 | 预测带宽骤降 >50% |
| **UNDER_ATTACK** | 0（禁用 Orca，回退 CUBIC） | 拒绝 | 异常检测超过阈值 |
| **RECOVERY** | [-1, 0.5]（谨慎恢复） | 允许 | 异常连续 50 次正常 |

### 3.3 训练管线

训练使用独立的 Python 管线（基于 PyTorch），训练完成后导出 ONNX 模型供 C++ 端加载推理。

#### 3.3.1 Orca SAC 训练 (`training/orca/`)

| 文件 | 职责 |
|------|------|
| `env.py` | CalibratedNetworkEnv：基于真实采集数据校准的仿真环境，4 种场景 profile |
| `sac.py` | Soft Actor-Critic 实现：Actor (策略) + 双 Critic (Q 函数) + 温度自动调整 |
| `network.py` | Actor/Critic 神经网络结构定义 |
| `replay_buffer.py` | 经验回放缓冲区 |
| `train.py` | 训练主循环，支持 ONNX 导出 |
| `export_onnx.py` | PyTorch → ONNX 模型导出 |

**仿真环境校准场景：**

| Profile | 描述 |
|---------|------|
| `local_normal` | 本地正常网络条件 |
| `local_tc` | 本地 + tc netem 流量整形 |
| `server_normal` | 服务器正常网络条件 |
| `server_tc` | 服务器 + tc netem 流量整形 |

#### 3.3.2 LSTM 带宽预测训练 (`training/bandwidth/`)

| 文件 | 职责 |
|------|------|
| `model.py` | LSTMBandwidthPredictor (input_dim=3, hidden_dim=64, num_layers=2) |
| `dataset.py` | 时间序列数据集：从 TCP 采样构造滑动窗口 |
| `train.py` | 监督学习，MSE 损失 |
| `export_onnx.py` | ONNX 导出 |

#### 3.3.3 Autoencoder 异常检测训练 (`training/anomaly/`)

| 文件 | 职责 |
|------|------|
| `model.py` | LSTMAutoencoder (input_dim=8, hidden_dim=32, latent_dim=8) |
| `dataset.py` | 正常/攻击流量样本加载 |
| `train.py` | 重构损失（MSE）训练 |
| `calibrate_threshold.py` | 在验证集上调优异常阈值 |
| `export_onnx.py` | ONNX 导出 |

#### 3.3.4 数据采集

```bash
# 1. 配置 Python 环境
conda env create -f training/environment.yml
conda activate neustack

# 2. 采集训练数据
sudo ./scripts/mac/collect.sh          # macOS
sudo ./scripts/linux/collect.sh        # Linux

# 3. 数据预处理
python scripts/python/csv_to_dataset.py collected_data/ training/real_data/

# 4. 训练模型
cd training/orca && python train.py
cd training/bandwidth && python train.py
cd training/anomaly && python train.py

# 5. 导出 ONNX
cd training/orca && python export_onnx.py
cd training/bandwidth && python export_onnx.py
cd training/anomaly && python export_onnx.py
```

---

## 4. 技术选型

### 4.1 编程语言

| 选项 | 选择 | 理由 |
|------|------|------|
| C++ | ✅ **C++20** | 零成本抽象、RAII、concepts/ranges 等现代特性、ONNX Runtime 原生支持 |
| C | ❌ | 缺乏抽象能力，跨平台代码冗余 |
| Rust | ❌ | ONNX Runtime 绑定不完善，跨语言 FFI 增加复杂度 |

### 4.2 构建系统

| 选项 | 选择 | 理由 |
|------|------|------|
| CMake | ✅ (≥ 3.20) | 跨平台标准、IDE 集成好、Ninja 支持 |
| Meson | ❌ | 社区较小 |
| Bazel | ❌ | 过于重量级 |

### 4.3 依赖

**C++ 协议栈（核心无外部依赖）：**

| 库 | 版本 | 用途 | 必需 |
|----|------|------|------|
| ONNX Runtime | ≥ 1.16 | AI 模型推理 | 可选（AI 功能） |
| Catch2 | v3 | 单元测试框架 | 可选（测试） |

**Python 训练：**

| 库 | 用途 |
|----|------|
| PyTorch | 模型定义与训练 |
| NumPy / Pandas | 数据处理 |
| Conda | 环境管理 (`training/environment.yml`) |

### 4.4 AI 框架

| 阶段 | 工具 | 用途 |
|------|------|------|
| 训练 | PyTorch（自定义 SAC/LSTM/AE 实现） | 模型训练 |
| 转换 | `torch.onnx.export()` | 模型导出为 ONNX |
| 推理 | ONNX Runtime C++ API | 高性能推理（< 1ms） |

---

## 5. 项目结构

```
NeuStack/
├── CMakeLists.txt                  # 主构建脚本 (C++20, Ninja)
├── cmake/                          # CMake 模块
│   ├── FindONNXRuntime.cmake       #   ONNX Runtime 自动发现
│   └── Platform.cmake              #   平台检测 (macOS/Linux/Windows)
├── include/neustack/               # C++ 公共头文件
│   ├── common/                     #   通用基础设施
│   │   ├── checksum.hpp            #     RFC 1071 校验和
│   │   ├── ip_addr.hpp             #     IP 地址工具
│   │   ├── isn_generator.hpp       #     TCP ISN 生成器
│   │   ├── ring_buffer.hpp         #     StreamBuffer 环形缓冲区
│   │   ├── spsc_queue.hpp          #     无锁 SPSC 队列
│   │   ├── log.hpp                 #     日志系统
│   │   └── string_utils.hpp        #     字符串工具
│   ├── hal/                        #   硬件抽象层
│   │   ├── device.hpp              #     NetDevice 抽象基类
│   │   ├── hal_macos.hpp           #     macOS utun
│   │   ├── hal_linux.hpp           #     Linux TUN/TAP
│   │   └── hal_windows.hpp         #     Windows Wintun
│   ├── net/                        #   网络层
│   │   ├── ipv4.hpp                #     IPv4 协议
│   │   ├── icmp.hpp                #     ICMP 协议
│   │   └── protocol_handler.hpp    #     协议分发接口
│   ├── transport/                  #   传输层
│   │   ├── tcp_layer.hpp           #     TCP 层协调器
│   │   ├── tcp_connection.hpp      #     TCP 连接管理
│   │   ├── tcp_tcb.hpp             #     TCP 控制块
│   │   ├── tcp_segment.hpp         #     TCP 段解析
│   │   ├── tcp_builder.hpp         #     TCP 段构造
│   │   ├── tcp_state.hpp           #     TCP 状态机
│   │   ├── tcp_seq.hpp             #     序列号管理
│   │   ├── tcp_reno.hpp            #     Reno 拥塞控制
│   │   ├── tcp_cubic.hpp           #     CUBIC 拥塞控制
│   │   ├── tcp_orca.hpp            #     Orca AI 拥塞控制
│   │   ├── udp.hpp                 #     UDP 协议
│   │   └── stream.hpp              #     流接口抽象
│   ├── app/                        #   应用层
│   │   ├── http_server.hpp         #     HTTP 服务器
│   │   ├── http_client.hpp         #     HTTP 客户端
│   │   ├── http_parser.hpp         #     HTTP 解析器
│   │   ├── http_types.hpp          #     HTTP 类型定义
│   │   └── dns_client.hpp          #     DNS 客户端
│   ├── metrics/                    #   指标采集
│   │   ├── global_metrics.hpp      #     全局原子指标
│   │   ├── tcp_sample.hpp          #     TCP 采样结构
│   │   ├── ai_features.hpp         #     AI 特征提取
│   │   ├── ai_action.hpp           #     AI 动作定义
│   │   ├── sample_exporter.hpp     #     采样 CSV 导出
│   │   └── metric_exporter.hpp     #     聚合指标导出
│   ├── ai/                         #   AI 智能面
│   │   ├── onnx_inference.hpp      #     ONNX Runtime 封装
│   │   ├── orca_model.hpp          #     Orca SAC Actor
│   │   ├── bandwidth_model.hpp     #     LSTM 带宽预测
│   │   ├── anomaly_model.hpp       #     LSTM-AE 异常检测
│   │   ├── ai_agent.hpp            #     NetworkAgent 决策层
│   │   └── intelligence_plane.hpp  #     双线程推理架构
│   └── neustack.hpp                #   统一入口头文件 (Facade)
├── src/                            # C++ 源代码实现
│   ├── neustack.cpp                #   Facade 实现
│   ├── common/                     #   通用模块实现
│   ├── hal/                        #   HAL 平台实现
│   ├── net/                        #   网络层实现
│   ├── transport/                  #   传输层实现
│   ├── app/                        #   应用层实现
│   ├── metrics/                    #   指标采集实现
│   └── ai/                         #   AI 推理实现
├── tests/                          # C++ 测试 (Catch2 v3)
│   ├── unit/                       #   16 个单元测试
│   ├── integration/                #   集成测试 (TCP 握手, HTTP 往返, ONNX, AI 模型)
│   └── benchmark/                  #   性能基准 (校验和, SPSC 队列, TCP 吞吐)
├── training/                       # Python AI 训练管线
│   ├── orca/                       #   SAC 拥塞控制训练 (16 文件)
│   ├── bandwidth/                  #   LSTM 带宽预测训练 (4 文件)
│   ├── anomaly/                    #   LSTM-AE 异常检测训练 (5 文件)
│   └── environment.yml             #   Conda 环境配置
├── models/                         # 预训练 ONNX 模型
│   ├── orca_actor.onnx             #   SAC Actor 网络
│   ├── bandwidth_predictor.onnx    #   LSTM 带宽预测器
│   └── anomaly_detector.onnx       #   LSTM-AE 异常检测器
├── scripts/                        # Shell/Python 脚本
│   ├── nat/                        #   NAT 配置 (setup/teardown)
│   ├── mac/                        #   macOS 数据采集/流量生成
│   ├── linux/                      #   Linux 数据采集
│   ├── download/                   #   下载脚本
│   │   ├── download_onnxruntime.sh #     下载 ONNX Runtime
│   │   └── download_wintun.sh      #     下载 Wintun DLL (Windows)
│   ├── python/                     #   Python 工具脚本
│   │   ├── csv_to_dataset.py       #     CSV → PyTorch 数据集转换
│   │   └── create_test_models.py   #     生成测试用 ONNX 模型
├── examples/                       # 示例程序
│   ├── minimal.cpp                 #   最小 HTTP 服务器 (26 行)
│   └── neustack_demo.cpp           #   完整交互式 Demo (CLI 参数, AI, CSV 采集)
└── docs/                           # 文档
    ├── project_whitepaper.md       #   本文档
    └── architecture.png            #   架构图
```

---

## 6. 构建与运行

### 6.1 依赖

**C++ 协议栈：**
- CMake >= 3.20
- C++20 编译器（Clang >= 14 / GCC >= 11 / MSVC 2019+）

**AI 训练（可选）：**
- Python 3.11 + PyTorch
- Conda（提供 `training/environment.yml`）

**可选：**
- [ONNX Runtime](https://onnxruntime.ai/) — C++ 端 AI 推理
- [Catch2 v3](https://github.com/catchorg/Catch2) — 单元测试

### 6.2 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `NEUSTACK_BUILD_TESTS` | ON | 编译测试 |
| `NEUSTACK_BUILD_EXAMPLES` | ON | 编译示例程序 |
| `NEUSTACK_BUILD_BENCHMARKS` | OFF | 编译性能基准测试 |
| `NEUSTACK_BUILD_TOOLS` | ON | 编译 `neustack-stat` 等 CLI 工具 |
| `NEUSTACK_ENABLE_ASAN` | OFF | 启用 Address Sanitizer |
| `NEUSTACK_ENABLE_UBSAN` | OFF | 启用 Undefined Behavior Sanitizer |
| `NEUSTACK_ENABLE_AI` | OFF | 启用 AI 拥塞控制（需要 ONNX Runtime） |
| `NEUSTACK_ENABLE_AF_XDP` | OFF | 启用 AF_XDP kernel-bypass backend（Linux，需要 libbpf + clang） |

### 6.3 构建与运行

```bash
# 基础构建
cmake -B build -G Ninja
cmake --build build --parallel

# 运行测试（注册数量会随 AI / benchmark 选项变化）
cd build && ctest --output-on-failure

# 启动协议栈（需要 root）
sudo ./build/examples/neustack_demo

# 配置 NAT（另一个终端）
sudo ./scripts/nat/setup_nat.sh --dev utun4
```

启用 AI 拥塞控制：

```bash
./scripts/download/download_onnxruntime.sh
cmake -B build -G Ninja -DNEUSTACK_ENABLE_AI=ON
cmake --build build --parallel
```

### 6.4 最小示例

```cpp
#include "neustack/neustack.hpp"
using namespace neustack;

int main() {
    auto stack = NeuStack::create();
    if (!stack) return 1;

    stack->http_server().get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n");
    });
    stack->http_server().listen(80);

    stack->run();  // Ctrl+C 退出
}
```

---

## 7. 测试策略

### 7.1 测试概览

NeuStack 使用 Catch2 v3 + CTest 管理测试。完整开发构建当前可发现 190+ 条测试用例；关闭 AI 或 benchmark 目标时，注册数量会相应减少。

| 类别 | 覆盖 |
|------|------|
| 单元测试 | common、HAL、transport、firewall、telemetry、app、AI 特征与状态机 |
| 集成测试 | TCP 三次握手/压力、HTTP 完整往返、防火墙规则与 E2E、AI runtime 与模型流水线 |
| 可选 benchmark | checksum、SPSC、TCP、AF_XDP datapath、E2E throughput（需 `NEUSTACK_BUILD_BENCHMARKS=ON`） |

### 7.2 单元测试详述

| 测试文件 | 覆盖模块 |
|----------|----------|
| `test_checksum.cpp` | RFC 1071 校验和计算正确性 |
| `test_ip_addr.cpp` | IP 地址解析与操作 |
| `test_tcp_seq.cpp` | 序列号比较、环绕处理 |
| `test_tcp_segment.cpp` | TCP 报文解析与序列化 |
| `test_tcp_builder.cpp` | TCP 报文从零构造 |
| `test_tcp_reno.cpp` | Reno 慢启动、拥塞避免、快速恢复 |
| `test_tcp_cubic.cpp` | CUBIC 三次函数窗口增长、TCP 友好模式 |
| `test_tcp_orca.cpp` | Orca 2^α 调制、α 回调、范围限制 |
| `test_tcp_sample.cpp` | TCP 采样结构与衍生指标 |
| `test_global_metrics.cpp` | 原子指标累加与快照 |
| `test_http_types.cpp` | HTTP 请求/响应结构体 |
| `test_http_parser.cpp` | HTTP/1.1 请求解析 |
| `test_ring_buffer.cpp` | StreamBuffer 读写与环绕 |
| `test_spsc_queue.cpp` | 无锁队列入队出队与满溢处理 |
| `test_ai_features.cpp` | 特征提取归一化正确性 |
| `test_ai_agent.cpp` | NetworkAgent 4 状态转换逻辑 |

### 7.3 性能基准

| 基准 | 测试内容 |
|------|----------|
| `bench_checksum.cpp` | 校验和计算吞吐量（23× 优化验证） |
| `bench_spsc_queue.cpp` | SPSC 队列生产者-消费者吞吐量 |
| `bench_tcp_throughput.cpp` | TCP 段处理吞吐量 |
| `bench_afxdp_datapath.cpp` | AF_XDP 数据路径各组件 micro-benchmark（UMEM alloc/free、XDP ring 批量、zero-copy send path、prefetch 效果、TCP header build） |
| `bench_e2e_throughput.cpp` | 端到端包吞吐量：kernel_udp / raw_socket / AF_XDP 三种 backend 对比（veth pair + network namespace 隔离）|

Benchmark 运行方式：

```bash
# micro-benchmark（带 JSON 输出）
./build/tests/bench_afxdp_datapath --json | python3 -m json.tool

# 批量运行 + 统计 + 图表生成
python3 scripts/bench/benchmark_runner.py --build-dir build/ --runs 5
python3 scripts/bench/plot_results.py --input bench_results/latest/summary.json

# E2E 吞吐测试（需要 root）
sudo bash scripts/bench/run_throughput_test.sh --duration 10 --runs 3
```

---

## 8. AF_XDP 高性能数据路径（v1.4 新增）

### 8.1 背景

Linux 传统 TUN/TAP 路径每包需要两次内存拷贝和一次 syscall，在高包率场景下成为瓶颈。AF_XDP 通过 BPF/XDP 程序在驱动层重定向报文到用户态共享内存（UMEM），绕过大部分内核协议栈开销。

### 8.2 实现架构

```
NIC 驱动
  │
  ▼  XDP 程序（BPF，在驱动 RX 钩子执行）
  │    XDP_REDIRECT → UMEM fill ring
  ▼
UMEM（mmap 共享内存，4096 × 4096B frames）
  │
  ├─ RX ring ──► userspace batch recv_batch()
  └─ TX ring ◄── userspace batch send_batch()
```

### 8.3 关键组件

| 组件 | 头文件 | 说明 |
|------|--------|------|
| `UMEMArea` | `hal/umem.hpp` | UMEM 生命周期管理，mmap + frame 分配器（FixedPool） |
| `XDPRing` | `hal/xdp_ring.hpp` | Fill / Completion / RX / TX 四环抽象，批量 ops |
| `BPFObject` | `hal/bpf_object.hpp` | BPF 程序加载、XDP attach/detach |
| `AFXDPDevice` | `hal/hal_linux_afxdp.hpp` | NetDevice 实现，整合以上组件，支持 generic/native 两种模式 |

### 8.4 运行模式

| 模式 | 配置 | 适用 NIC | 内存拷贝数 |
|------|------|----------|------------|
| Generic (SKB copy) | `zero_copy=false` | 所有 Linux NIC | 1 次（SKB→UMEM）|
| Native zero-copy | `zero_copy=true, force_native_mode=true` | Intel i40e/ice/igc/mlx5 等 | 0 次 |

> **当前测试环境**：Realtek r8169（无原生 XDP 支持），使用 generic copy mode。

### 8.5 性能结果（v1.4）

| 指标 | 数值 | 说明 |
|------|------|------|
| E2E 吞吐（AF_XDP generic） | **1.18 Mpps** | vs 内核 UDP 0.82 Mpps（**1.45×**）|
| Zero-copy send path | **52 ns/pkt** | vs 传统 3-copy 162 ns/pkt（**3.1×**）|
| XDP ring 批量化收益 | batch=128 → **0.55 ns/op** | vs batch=1 时 2.37 ns/op（**4.3×**）|
| UMEM alloc+free | **0.46 ns/op** | 接近硬件极限 |

---

## 9. 技术风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| AI 推理延迟过高 | 影响 TCP 实时性 | 双线程分离：AI 推理不阻塞数据面；推理频率分层（10ms/100ms/1s） |
| RL 模型在真实网络中泛化差 | Orca 效果不佳 | CalibratedNetworkEnv 基于真实采集数据校准；4 种场景 profile 域随机化 |
| 异常检测误报 | 误切回 CUBIC | NetworkAgent 状态机要求连续 50 次正常才恢复；可配置阈值 |
| 跨平台 HAL 差异大 | 维护成本高 | NetDevice 抽象接口 + 平台条件编译；CMake Platform.cmake 自动检测 |
| 无锁队列在高负载下的行为 | 丢失 AI 动作 | SPSCQueue 固定 power-of-2 大小，失败时 graceful degrade |

---

## 10. 参考文献

### RFC 规范

- RFC 791: Internet Protocol (IPv4)
- RFC 792: Internet Control Message Protocol (ICMP)
- RFC 768: User Datagram Protocol (UDP)
- RFC 9293: Transmission Control Protocol (TCP) — 替代 RFC 793
- RFC 1071: Computing the Internet Checksum
- RFC 5681: TCP Congestion Control
- RFC 6298: Computing TCP's Retransmission Timer
- RFC 8312: CUBIC for Fast Long-Distance Networks

### 学术论文

- Abbasloo, S., et al. (2020). *Classic Meets Modern: a Pragmatic Learning-Based Congestion Control for the Internet*. SIGCOMM. — **Orca 原论文**
- Haarnoja, T., et al. (2018). *Soft Actor-Critic: Off-Policy Maximum Entropy Deep Reinforcement Learning with a Stochastic Actor*. ICML. — **SAC 算法**
- Jay, N., et al. (2019). *A Deep Reinforcement Learning Perspective on Internet Congestion Control*. ICML.
- Winstein, K., & Balakrishnan, H. (2013). *TCP ex Machina: Computer-Generated Congestion Control*. SIGCOMM.
- Yan, F., et al. (2018). *Pantheon: the training ground for Internet congestion-control research*. ATC.

### 开源项目

- [lwIP](https://savannah.nongnu.org/projects/lwip/) — 轻量级 TCP/IP 协议栈
- [Seastar](https://github.com/scylladb/seastar) — 高性能用户态网络库
- [mTCP](https://github.com/mtcp-stack/mtcp) — 多核用户态 TCP 栈
- [ONNX Runtime](https://onnxruntime.ai/) — 高性能 ML 推理引擎

---

## 10. 附录

### A. 开发环境配置

**macOS:**

```bash
# 安装依赖
brew install cmake ninja catch2

# 克隆与构建
git clone <repo-url> NeuStack && cd NeuStack
cmake -B build -G Ninja
cmake --build build

# 运行测试
cd build && ctest --output-on-failure

# 运行协议栈（需要 root 创建 utun 设备）
sudo ./build/examples/neustack_demo --ip 192.168.100.2

# 配置 NAT（另一个终端）
sudo ./scripts/nat/setup_nat.sh --dev utun4
```

**Linux:**

```bash
# 安装依赖 (Ubuntu/Debian)
sudo apt install cmake ninja-build g++ catch2

# 加载 TUN 模块
sudo modprobe tun

# 构建和运行同 macOS
```

**启用 AI 功能：**

```bash
# 下载 ONNX Runtime
./scripts/download/download_onnxruntime.sh

# 带 AI 构建
cmake -B build -G Ninja -DNEUSTACK_ENABLE_AI=ON
cmake --build build

# 运行 (指定模型目录)
sudo ./build/examples/neustack_demo --models models/
```

### B. NAT 配置（本地测试）

macOS 上通过 pf 实现 NAT，让协议栈经由 utun 设备访问公网：

```bash
# 启动 NAT
sudo ./scripts/nat/setup_nat.sh --dev utun4

# 清理 NAT
sudo ./scripts/nat/teardown_nat.sh --dev utun4
```

### C. 代码统计

由 `cloc` 工具统计（不含空行和注释）：

| 类别 | 文件数 | 代码行数 |
|------|--------|---------|
| C++ 源文件 (`.cpp`) | 50 | 6,519 |
| Python (`.py`) | 18 | 3,225 |
| C/C++ 头文件 (`.hpp`) | 46 | 2,887 |
| Shell 脚本 (`.sh`) | 14 | 1,368 |
| CMake | 5 | 291 |
| 其他 (JSON, YAML 等) | 9 | 331 |
| **合计** | **142** | **14,621** |

---

*文档结束*
