# NeuStack 项目白皮书

> **Version:** 1.0
> **Last Updated:** 2026-01-24
> **Status:** Draft

---

## 1. 项目概述

### 1.1 项目定位

**NeuStack** 是一个从零实现的、高性能的、模块化的**用户态 TCP/IP 协议栈**。区别于传统内核协议栈的黑盒实现，NeuStack 通过以下三大核心特性实现差异化：

| 特性 | 描述 |
|------|------|
| **跨平台 HAL** | 硬件抽象层屏蔽 macOS/Linux/Windows 底层差异 |
| **AI 拥塞控制** | 基于强化学习的智能 CWND 调节，替代传统 Cubic/BBR |
| **用户态实现** | 完全运行在用户空间，便于调试、定制和部署 |

### 1.2 解决的问题

1. **内核协议栈的不透明性**：无法细粒度控制 TCP 行为
2. **传统 CC 算法的局限性**：固定数学模型难以适应复杂网络环境
3. **跨平台开发的碎片化**：不同系统的网络接口差异巨大
4. **高性能场景的瓶颈**：内核态与用户态的频繁切换带来开销

### 1.3 目标用户

- 需要定制网络行为的应用开发者
- 网络协议研究人员
- AI/ML 与网络交叉领域的研究者

---

## 2. 系统架构

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application Layer                          │
│                   ┌─────────────────────┐                       │
│                   │   NeuSocket API     │  (POSIX-like)         │
│                   │  connect/send/recv  │                       │
│                   └──────────┬──────────┘                       │
├──────────────────────────────┼──────────────────────────────────┤
│                      Transport Layer                            │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    TCP Engine                             │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐   │   │
│  │  │ State       │  │ Sliding     │  │ Retransmission  │   │   │
│  │  │ Machine     │  │ Window      │  │ Timer           │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────┘   │   │
│  │                          │                                │   │
│  │              ┌───────────▼───────────┐                   │   │
│  │              │   AI-CC Module        │  ← ONNX Runtime   │   │
│  │              │   (Congestion Ctrl)   │                   │   │
│  │              └───────────────────────┘                   │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│                       Network Layer                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │
│  │    IPv4      │  │    ICMP      │  │       ARP            │   │
│  │  Routing     │  │  Echo/Reply  │  │   Cache/Resolution   │   │
│  └──────────────┘  └──────────────┘  └──────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│                 Hardware Abstraction Layer                      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                   NetDevice (Abstract)                    │   │
│  ├──────────────┬──────────────────┬───────────────────────┤   │
│  │ macOS: utun  │  Linux: TAP/TUN  │  Windows: Wintun      │   │
│  └──────────────┴──────────────────┴───────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 模块职责详述

#### 2.2.1 Socket API Layer (`include/socket.hpp`)

提供应用层接口，屏蔽底层协议栈复杂性。

```cpp
// 核心接口定义
class NeuSocket {
public:
    // 生命周期管理
    static NeuSocket* create(int domain, int type, int protocol);
    int close();

    // 连接管理
    int bind(const SockAddr* addr, socklen_t len);
    int listen(int backlog);
    NeuSocket* accept(SockAddr* addr, socklen_t* len);
    int connect(const SockAddr* addr, socklen_t len);

    // 数据传输
    ssize_t send(const void* buf, size_t len, int flags);
    ssize_t recv(void* buf, size_t len, int flags);

    // 配置
    int setsockopt(int level, int optname, const void* optval, socklen_t len);
    int getsockopt(int level, int optname, void* optval, socklen_t* len);
};
```

#### 2.2.2 TCP Engine (`src/core/tcp/`)

TCP 核心实现，遵循 RFC 793/5681/6298 规范。

| 子模块 | 文件 | 职责 |
|--------|------|------|
| 状态机 | `tcp_state.cpp` | 11 种 TCP 状态转换 (CLOSED→LISTEN→SYN_RCVD→...) |
| 滑动窗口 | `tcp_window.cpp` | 发送窗口、接收窗口、流量控制 |
| 重传机制 | `tcp_retransmit.cpp` | RTO 计算、快速重传、选择性确认 (SACK) |
| 序列号 | `tcp_seq.cpp` | ISN 生成、序列号环绕处理 |
| 定时器 | `tcp_timer.cpp` | 重传定时器、TIME_WAIT 定时器、Keepalive |

**TCP 状态机转换图：**

```
                              ┌──────────────┐
                              │    CLOSED    │
                              └──────┬───────┘
                    ┌────────────────┼────────────────┐
                    │ passive open   │                │ active open
                    ▼                │                ▼
             ┌──────────┐            │         ┌──────────────┐
             │  LISTEN  │            │         │   SYN_SENT   │
             └────┬─────┘            │         └──────┬───────┘
          rcv SYN │                  │                │ rcv SYN+ACK
       send SYN+ACK                  │                │ send ACK
                  ▼                  │                ▼
             ┌──────────┐            │         ┌──────────────┐
             │ SYN_RCVD │────────────┼────────▶│ ESTABLISHED  │
             └──────────┘  rcv ACK   │         └──────────────┘
                                     │                │
                                    ...              ...
```

#### 2.2.3 AI-CC Module (`src/core/ai/`)

基于强化学习的智能拥塞控制模块。

**输入特征 (State Vector):**

| 特征 | 类型 | 描述 |
|------|------|------|
| `srtt` | float32 | 平滑往返时间 (ms) |
| `rtt_var` | float32 | RTT 变化量 (ms) |
| `cwnd` | uint32 | 当前拥塞窗口 (bytes) |
| `ssthresh` | uint32 | 慢启动阈值 (bytes) |
| `loss_rate` | float32 | 丢包率 (0.0-1.0) |
| `throughput` | float32 | 吞吐量 (Mbps) |
| `inflight` | uint32 | 在途数据量 (bytes) |
| `rto_count` | uint32 | RTO 超时次数 |

**输出动作 (Action):**

| 动作 | 取值范围 | 描述 |
|------|----------|------|
| `cwnd_delta` | [-0.5, 2.0] | CWND 调整比例 |

**模型规格：**

```yaml
Framework: PyTorch → ONNX
Algorithm: PPO (Proximal Policy Optimization)
Input Shape: [batch_size, 8]
Output Shape: [batch_size, 1]
Inference Engine: ONNX Runtime (C++ API)
Target Latency: < 1ms per inference
```

#### 2.2.4 Network Layer (`src/core/net/`)

| 模块 | 文件 | 核心功能 |
|------|------|----------|
| IPv4 | `ipv4.cpp` | 报文解析、校验和计算、分片重组、路由查找 |
| ICMP | `icmp.cpp` | Echo Request/Reply、Destination Unreachable |
| ARP | `arp.cpp` | MAC 地址解析、ARP 缓存管理、ARP 代理 |

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
  ┌───────────────┐     ┌──────────────┐
  │ 分片重组      │────▶│ 丢弃碎片     │ (超时)
  └───────┬───────┘     └──────────────┘
          │ (完整报文)
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

#### 2.2.5 Hardware Abstraction Layer (`src/hal/`)

```cpp
// 抽象基类定义
class NetDevice {
public:
    virtual ~NetDevice() = default;

    // 生命周期
    virtual int open() = 0;
    virtual int close() = 0;

    // 数据收发
    virtual ssize_t send(const uint8_t* data, size_t len) = 0;
    virtual ssize_t recv(uint8_t* buf, size_t len, int timeout_ms) = 0;

    // 配置
    virtual int set_ip(const char* ip, const char* netmask) = 0;
    virtual int set_mtu(uint16_t mtu) = 0;
    virtual int get_fd() const = 0;  // for select/poll/epoll

    // 工厂方法
    static std::unique_ptr<NetDevice> create();
};
```

**平台特定实现：**

| 平台 | 文件 | 底层技术 | 关键系统调用 |
|------|------|----------|--------------|
| macOS | `hal_macos.cpp` | utun | `socket(PF_SYSTEM, ...)`, `ioctl(CTLIOCGINFO)` |
| Linux | `hal_linux.cpp` | TAP/TUN | `open("/dev/net/tun")`, `ioctl(TUNSETIFF)` |
| Windows | `hal_windows.cpp` | Wintun | `WintunCreateAdapter()`, Ring Buffer API |

---

## 3. 技术选型

### 3.1 编程语言

| 选项 | 选择 | 理由 |
|------|------|------|
| C++ | ✅ **C++17** | 零成本抽象、RAII、现代内存管理、ONNX Runtime 原生支持 |
| C | ❌ | 缺乏抽象能力，跨平台代码冗余 |
| Rust | ❌ | 生态不够成熟，ONNX Runtime 绑定不完善 |

### 3.2 构建系统

| 选项 | 选择 | 理由 |
|------|------|------|
| CMake | ✅ | 跨平台标准、IDE 集成好、依赖管理成熟 |
| Meson | ❌ | 社区较小 |
| Bazel | ❌ | 过于重量级 |

### 3.3 依赖库

| 库 | 版本 | 用途 |
|----|------|------|
| ONNX Runtime | ≥1.16 | AI 模型推理 |
| spdlog | ≥1.12 | 高性能日志 |
| Catch2 | ≥3.4 | 单元测试 |
| CLI11 | ≥2.3 | 命令行解析 |

### 3.4 AI 框架

| 阶段 | 工具 | 用途 |
|------|------|------|
| 训练 | PyTorch + Stable Baselines3 | 强化学习模型训练 |
| 转换 | torch.onnx.export() | 模型导出为 ONNX |
| 推理 | ONNX Runtime C++ | 高性能推理 |

---

## 4. 项目结构

```
NeuStack/
├── CMakeLists.txt              # 主构建脚本
├── cmake/                      # CMake 模块
│   ├── FindONNXRuntime.cmake
│   └── Platform.cmake          # 平台检测
├── include/                    # 公共头文件
│   ├── neustack/
│   │   ├── common/
│   │   │   ├── types.hpp       # 基础类型定义
│   │   │   ├── buffer.hpp      # 零拷贝缓冲区
│   │   │   ├── endian.hpp      # 字节序转换
│   │   │   └── result.hpp      # 错误处理 (Expected<T, E>)
│   │   ├── hal/
│   │   │   └── device.hpp      # NetDevice 抽象基类
│   │   ├── net/
│   │   │   ├── ethernet.hpp
│   │   │   ├── ipv4.hpp
│   │   │   ├── icmp.hpp
│   │   │   └── arp.hpp
│   │   ├── transport/
│   │   │   ├── tcp.hpp
│   │   │   ├── tcp_state.hpp
│   │   │   └── tcp_option.hpp
│   │   ├── ai/
│   │   │   └── cc_model.hpp    # 拥塞控制模型接口
│   │   └── socket.hpp          # NeuSocket API
│   └── neustack.hpp            # 统一入口头文件
├── src/
│   ├── main.cpp                # 程序入口
│   ├── common/
│   │   ├── buffer.cpp
│   │   └── checksum.cpp        # 校验和计算
│   ├── hal/
│   │   ├── device.cpp          # 工厂实现
│   │   ├── hal_macos.cpp       # macOS utun
│   │   ├── hal_linux.cpp       # Linux TAP
│   │   └── hal_windows.cpp     # Windows Wintun
│   ├── net/
│   │   ├── ethernet.cpp
│   │   ├── ipv4.cpp
│   │   ├── icmp.cpp
│   │   └── arp.cpp
│   ├── transport/
│   │   ├── tcp_conn.cpp        # TCP 连接管理
│   │   ├── tcp_state.cpp       # 状态机
│   │   ├── tcp_window.cpp      # 滑动窗口
│   │   ├── tcp_retransmit.cpp  # 重传
│   │   └── tcp_timer.cpp       # 定时器
│   ├── ai/
│   │   ├── cc_model.cpp        # ONNX 推理封装
│   │   ├── cc_classic.cpp      # 经典算法 (Cubic/BBR) 作为对照
│   │   └── cc_factory.cpp      # CC 算法工厂
│   └── socket/
│       └── socket.cpp          # NeuSocket 实现
├── models/                     # AI 模型文件
│   ├── cc_ppo_v1.onnx
│   └── README.md               # 模型说明
├── tests/
│   ├── unit/                   # 单元测试
│   │   ├── test_checksum.cpp
│   │   ├── test_tcp_state.cpp
│   │   └── test_sliding_window.cpp
│   ├── integration/            # 集成测试
│   │   ├── test_ping.cpp
│   │   └── test_tcp_handshake.cpp
│   └── benchmark/              # 性能测试
│       └── bench_throughput.cpp
├── scripts/
│   ├── setup_macos.sh          # macOS 网卡配置
│   ├── setup_linux.sh          # Linux 网卡配置
│   └── train_model.py          # AI 模型训练脚本
├── docs/
│   ├── project_whitepaper.md   # 本文档
│   ├── architecture.md         # 架构详解
│   └── api_reference.md        # API 文档
└── third_party/                # 第三方依赖
    └── wintun/                 # Windows Wintun SDK
```

---

## 5. 开发里程碑

### Phase 1: 基础设施 (Foundation)

**目标：** 搭建开发环境，实现 HAL 层，能够收发原始 IP 报文

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| 项目初始化 | CMakeLists.txt, 目录结构 | `cmake -B build && cmake --build build` 成功 |
| 基础类型定义 | types.hpp, endian.hpp | 编译通过 |
| 零拷贝缓冲区 | buffer.hpp/cpp | 单元测试通过 |
| macOS HAL | hal_macos.cpp | 成功创建 utun 设备，能收发原始数据 |
| 主循环框架 | main.cpp | 事件循环能持续运行 |

### Phase 2: 网络层 - IP (IPv4)

**目标：** 实现 IPv4 报文解析和发送

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| IPv4 头解析 | ipv4.hpp/cpp | 正确解析 Wireshark 抓包数据 |
| 校验和计算 | checksum.cpp | 通过 RFC 测试向量验证 |
| IPv4 报文构造 | ipv4.cpp | 构造的报文能被 Wireshark 正确解析 |
| 分片重组 | ipv4_fragment.cpp | 能处理分片报文 (可后续实现) |
| 简单路由表 | route.cpp | 支持默认路由和直连路由 |

### Phase 3: 网络层 - ICMP

**目标：** 实现 ICMP Echo，能响应 ping 请求

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| ICMP 解析 | icmp.hpp/cpp | 正确解析 Echo Request |
| ICMP Echo Reply | icmp.cpp | `ping <虚拟IP>` 收到回复 |
| ICMP 错误消息 | icmp.cpp | Destination Unreachable 等 |

### Phase 4: 链路层 - ARP

**目标：** 实现 ARP 协议 (如果使用 TAP 设备需要)

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| ARP 报文解析 | arp.hpp/cpp | 正确解析 ARP Request/Reply |
| ARP 缓存 | arp_cache.cpp | 缓存表管理、超时清理 |
| ARP 请求发送 | arp.cpp | 能主动发起 ARP 请求 |
| ARP 代理 | arp.cpp | 响应对虚拟 IP 的 ARP 请求 |

> **注意：** macOS utun 是 L3 设备 (点对点)，不需要 ARP。Linux TAP 是 L2 设备，需要 ARP。

### Phase 5: 传输层 - UDP

**目标：** 实现 UDP 协议，提供无连接数据报服务

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| UDP 解析 | udp.hpp/cpp | 正确解析 UDP 报文 |
| UDP 发送 | udp.cpp | 能发送 UDP 数据报 |
| 端口管理 | port_manager.cpp | 端口绑定、复用 |
| UDP Socket | udp_socket.cpp | 简单的 send/recv 接口 |
| 集成测试 | test_udp.cpp | 与 netcat 互通测试 |

### Phase 6: 传输层 - TCP (后续)

**目标：** 实现完整 TCP 协议

#### Phase 6.1: TCP 连接管理
| 任务 | 产出 | 验收标准 |
|------|------|----------|
| TCP 状态机 | tcp_state.cpp | 11 种状态，单元测试全覆盖 |
| 序列号管理 | tcp_seq.cpp | ISN 安全随机，环绕处理正确 |
| 三次握手 | tcp_conn.cpp | Wireshark 验证 SYN/SYN-ACK/ACK |
| 四次挥手 | tcp_conn.cpp | 正常关闭，TIME_WAIT 正确 |
| TCP 选项解析 | tcp_option.cpp | MSS, Window Scale, SACK Permitted |

#### Phase 6.2: TCP 可靠传输
| 任务 | 产出 | 验收标准 |
|------|------|----------|
| 发送缓冲区 | tcp_send_buffer.cpp | 数据排队、按序发送 |
| 接收缓冲区 | tcp_recv_buffer.cpp | 乱序重组、按序交付 |
| 滑动窗口 | tcp_window.cpp | 流量控制正确 |
| 重传定时器 | tcp_timer.cpp | RTO 按 RFC 6298 计算 |
| 快速重传 | tcp_retransmit.cpp | 3 dup ACK 触发 |
| SACK | tcp_sack.cpp | 选择性确认 (可选) |

#### Phase 6.3: TCP 拥塞控制
| 任务 | 产出 | 验收标准 |
|------|------|----------|
| 慢启动 | cc_classic.cpp | CWND 指数增长 |
| 拥塞避免 | cc_classic.cpp | CWND 线性增长 |
| 快速恢复 | cc_classic.cpp | 半窗重传 |
| Cubic | cc_cubic.cpp | 完整 Cubic 实现 |

### Phase 7: AI 拥塞控制 (AI-CC)

**目标：** 集成 ONNX Runtime，实现智能拥塞控制

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| 训练环境搭建 | train_model.py | 网络模拟器 + RL 环境 |
| PPO 模型训练 | cc_ppo.onnx | 模型收敛，奖励稳定 |
| ONNX 推理集成 | cc_model.cpp | 推理延迟 < 1ms |
| CC 模块切换 | cc_factory.cpp | 运行时切换 Cubic/AI |
| A/B 性能对比 | bench_cc.cpp | 弱网环境下优于 Cubic |

### Phase 8: Socket API & 跨平台

**目标：** 提供完整 Socket API，支持 Linux/Windows

| 任务 | 产出 | 验收标准 |
|------|------|----------|
| NeuSocket API | socket.cpp | POSIX 兼容接口 |
| 阻塞/非阻塞模式 | socket.cpp | 两种模式都支持 |
| Linux HAL | hal_linux.cpp | Linux 下全部测试通过 |
| Windows HAL | hal_windows.cpp | Windows 下全部测试通过 |
| Echo Server 示例 | examples/echo.cpp | 完整工作示例 |

---

## 6. 技术风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| AI 模型推理延迟过高 | 影响 TCP 性能 | 模型量化、批处理、降采样 |
| TCP 状态机实现复杂 | 开发周期长 | 分阶段实现，先支持基础场景 |
| 跨平台 HAL 差异大 | 维护成本高 | 良好的抽象设计，充分的平台测试 |
| RL 模型在真实网络中泛化差 | AI-CC 效果不佳 | 域随机化训练，在线微调 |

---

## 7. 性能目标

| 指标 | 目标值 | 测试方法 |
|------|--------|----------|
| 单连接吞吐量 | ≥ 500 Mbps | iperf3 对照测试 |
| AI 推理延迟 | < 1 ms (p99) | 性能采样 |
| 内存占用 | < 50 MB (1000 连接) | Valgrind massif |
| CPU 利用率 | < 30% (单核, 1Gbps) | perf 采样 |

---

## 8. 测试策略

### 8.1 单元测试

- 覆盖所有核心模块
- 使用 Catch2 框架
- CI 自动运行

### 8.2 集成测试

- Wireshark 抓包验证协议正确性
- 与 Linux 内核协议栈互操作测试

### 8.3 性能测试

- iperf3 吞吐量测试
- tc netem 模拟弱网环境
- AI-CC vs Cubic vs BBR 对比

### 8.4 Fuzz 测试

- 使用 libFuzzer 对报文解析进行模糊测试
- 发现潜在的内存安全问题

---

## 9. 参考文献

### RFC 规范

- RFC 793: Transmission Control Protocol
- RFC 1122: Requirements for Internet Hosts
- RFC 5681: TCP Congestion Control
- RFC 6298: Computing TCP's Retransmission Timer
- RFC 6675: A Conservative Loss Recovery Algorithm (SACK)
- RFC 8312: CUBIC for Fast Long-Distance Networks

### 学术论文

- Winstein, K., & Balakrishnan, H. (2013). *TCP ex Machina: Computer-Generated Congestion Control*. SIGCOMM.
- Jay, N., et al. (2019). *A Deep Reinforcement Learning Perspective on Internet Congestion Control*. ICML.
- Yan, F., et al. (2018). *Pantheon: the training ground for Internet congestion-control research*. ATC.

### 开源项目

- [lwIP](https://savannah.nongnu.org/projects/lwip/) - 轻量级 TCP/IP 协议栈
- [Seastar](https://github.com/scylladb/seastar) - 高性能网络库
- [mTCP](https://github.com/mtcp-stack/mtcp) - 多核用户态 TCP 栈

---

## 10. 附录

### A. 开发环境配置

**macOS:**

```bash
# 安装依赖
brew install cmake onnxruntime spdlog catch2

# 克隆项目
git clone https://github.com/xxx/NeuStack.git
cd NeuStack

# 构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)

# 运行（需要 root 权限创建 utun）
sudo ./build/neustack
```

**Linux:**

```bash
# 安装依赖 (Ubuntu/Debian)
sudo apt install cmake libonnxruntime-dev libspdlog-dev catch2

# 加载 TUN 模块
sudo modprobe tun

# 构建和运行同 macOS
```

### B. 网卡配置示例 (macOS)

```bash
#!/bin/bash
# scripts/setup_macos.sh

UTUN_NAME="utun9"
IP_ADDR="10.0.0.1"
NETMASK="255.255.255.0"
PEER_ADDR="10.0.0.2"

# NeuStack 启动后会自动创建 utun 设备
# 此脚本用于配置路由

sudo ifconfig $UTUN_NAME $IP_ADDR $PEER_ADDR netmask $NETMASK up
sudo route add -net 10.0.0.0/24 -interface $UTUN_NAME
```

### C. 拥塞控制模型训练

```python
# scripts/train_model.py (伪代码)

import gymnasium as gym
from stable_baselines3 import PPO
import torch

# 自定义网络模拟环境
class NetworkEnv(gym.Env):
    def __init__(self):
        # 状态空间: [srtt, rtt_var, cwnd, ssthresh, loss_rate, throughput, inflight, rto_count]
        self.observation_space = gym.spaces.Box(low=0, high=np.inf, shape=(8,))
        # 动作空间: cwnd 调整比例
        self.action_space = gym.spaces.Box(low=-0.5, high=2.0, shape=(1,))

    def step(self, action):
        # 模拟网络传输，返回 (obs, reward, done, truncated, info)
        ...

# 训练
env = NetworkEnv()
model = PPO("MlpPolicy", env, verbose=1)
model.learn(total_timesteps=1_000_000)

# 导出 ONNX
dummy_input = torch.randn(1, 8)
torch.onnx.export(model.policy, dummy_input, "models/cc_ppo_v1.onnx")
```

---

*文档结束*
