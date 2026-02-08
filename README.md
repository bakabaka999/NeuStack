<p align="center">
  <h1 align="center">NeuStack</h1>
  <p align="center">
    <strong>从零实现的跨平台用户态 TCP/IP 协议栈，集成 AI 拥塞控制智能面</strong>
  </p>
  <p align="center">
    <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"></a>
    <a href="https://www.python.org/"><img src="https://img.shields.io/badge/Python-3.11-3776AB.svg" alt="Python 3.11"></a>
    <a href="https://pytorch.org/"><img src="https://img.shields.io/badge/PyTorch-SAC%20%2F%20LSTM%20%2F%20AE-EE4C2C.svg" alt="PyTorch"></a>
    <a href="https://onnxruntime.ai/"><img src="https://img.shields.io/badge/ONNX%20Runtime-inference-7B68EE.svg" alt="ONNX Runtime"></a>
    <img src="https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey.svg" alt="Platform">
    <img src="https://img.shields.io/badge/tests-42%20passed-brightgreen.svg" alt="Tests">
    <img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License">
  </p>
</p>

---

## 项目简介

NeuStack 是一个**完全从零实现**的用户态 TCP/IP 协议栈。C++ 实现协议栈核心（~10,000 行），Python 实现 AI 模型训练管线（~4,000 行），Shell 脚本实现数据采集与环境配置（~2,700 行）。

### 核心特性

| 类别 | 内容 |
|------|------|
| **协议栈** | IPv4 / ICMP / UDP / TCP / HTTP 1.1 / DNS |
| **拥塞控制** | Reno · CUBIC · **Orca (SAC 强化学习)** |
| **AI 智能面** | 带宽预测 (LSTM) · 异常检测 (Autoencoder) · 智能拥塞控制 (SAC) |
| **NetworkAgent** | 4 状态决策层，协调 3 个模型，策略性 clamp / 回退 / 连接控制 |
| **跨平台 HAL** | macOS (utun) · Linux (TUN/TAP) · Windows (Wintun) |
| **训练管线** | PyTorch 训练 → ONNX 导出 → C++ ONNX Runtime 推理 |
| **数据采集** | 真实网络数据采集脚本 · CSV 导出 · 自动化训练数据集生成 |
| **测试** | 42 个测试（单元 / 集成 / 基准） · 零拷贝校验和 (23x 优化) |

## 系统架构

<p align="center">
  <img src="docs/architecture.png" alt="NeuStack System Architecture" width="720">
</p>

> 如果图片未显示，请参考下方文字版架构图。

<details>
<summary>文字版架构图</summary>

```
┌─────────────────────────────────────────────────────────┐
│                    应用层 (Application)                    │
│             HTTP Server/Client  ·  DNS Client             │
├─────────────────────────────────────────────────────────┤
│                    传输层 (Transport)                      │
│         TCP (Reno / CUBIC / Orca)  ·  UDP                 │
├─────────────────────────────────────────────────────────┤
│                    网络层 (Network)                        │
│                   IPv4  ·  ICMP                           │
├─────────────────────────────────────────────────────────┤
│                  硬件抽象层 (HAL)                          │
│          macOS utun  ·  Linux TUN  ·  Wintun              │
└─────────────────────────────────────────────────────────┘
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
                      │  数据采集 → 预处理 → 离线/在线训练   │
                      └──────────────────────────────────┘
```

</details>

## 快速开始

### 依赖

**C++ 协议栈：**
- CMake >= 3.20
- C++20 编译器 (Clang >= 14 / GCC >= 11 / MSVC 2019+)

**AI 训练（可选）：**
- Python 3.11 + PyTorch
- Conda（推荐，提供 `environment.yml`）

**可选：**
- [ONNX Runtime](https://onnxruntime.ai/) — C++ 端 AI 推理
- [Catch2 v3](https://github.com/catchorg/Catch2) — 单元测试

### 构建与运行

```bash
# 构建
cmake -B build -G Ninja
cmake --build build

# 运行测试 (42 个)
cd build && ctest --output-on-failure

# 启动协议栈 (需要 root)
sudo ./build/examples/neustack_demo

# 另一个终端：配置 NAT
sudo ./scripts/nat/setup_nat.sh --dev utun4
```

启用 AI 拥塞控制：

```bash
./scripts/download_onnxruntime.sh
cmake -B build -G Ninja -DNEUSTACK_ENABLE_AI=ON
cmake --build build
```

### 最小示例

```cpp
#include "neustack/neustack.hpp"
using namespace neustack;

int main() {
    auto stack = NeuStack::create();

    stack->http_server().get("/", [](const HttpRequest &) {
        return HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n");
    });
    stack->http_server().listen(80);

    stack->run();  // Ctrl+C 退出
}
```

## AI 智能面 & NetworkAgent

NeuStack 的 AI 子系统不是简单地把模型输出直接用于拥塞控制，而是通过一个**决策层 Agent（`NetworkAgent`）** 协调三个模型，根据网络态势做出策略性决策。

### 三个模型

| 模型 | 算法 | 输入 | 输出 | 用途 |
|------|------|------|------|------|
| **Orca** | SAC (强化学习) | 7 维网络状态 | CWND 调节系数 α ∈ [-1, 1] | 拥塞窗口智能调节 |
| **带宽预测** | LSTM (2层) | 30 步 × 3 维时序 | 预测带宽 (bytes/s) | 为 Orca 提供前瞻性带宽估计 |
| **异常检测** | Autoencoder | 7 维网络特征 | 重构误差 | 检测网络异常，触发保守策略 |

### NetworkAgent 决策层

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

### 训练流程

```bash
# 1. 配置 Python 环境
conda env create -f training/environment.yml
conda activate neustack

# 2. 采集训练数据
sudo ./scripts/mac/collect.sh          # macOS
sudo ./scripts/linux/collect.sh        # Linux

# 3. 数据预处理
python scripts/csv_to_dataset.py collected_data/ training/real_data/

# 4. 训练模型
cd training/orca && python train.py          # SAC 拥塞控制
cd training/bandwidth && python train.py     # LSTM 带宽预测
cd training/anomaly && python train.py       # Autoencoder 异常检测

# 5. 导出 ONNX
cd training/orca && python export_onnx.py
cd training/bandwidth && python export_onnx.py
cd training/anomaly && python export_onnx.py
```

训练完成后模型自动输出到 `models/` 目录，C++ 端通过 ONNX Runtime 加载推理。

## 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `NEUSTACK_BUILD_TESTS` | ON | 编译单元测试 |
| `NEUSTACK_BUILD_EXAMPLES` | ON | 编译示例程序 |
| `NEUSTACK_BUILD_BENCHMARKS` | OFF | 编译性能基准测试 |
| `NEUSTACK_ENABLE_ASAN` | OFF | 启用 Address Sanitizer |
| `NEUSTACK_ENABLE_AI` | OFF | 启用 AI 拥塞控制 (需要 ONNX Runtime) |

## 项目结构

```
NeuStack/
├── include/neustack/          # C++ 公共头文件
│   ├── common/                #   通用工具 (checksum, ip_addr, ring_buffer, spsc_queue)
│   ├── hal/                   #   硬件抽象层接口
│   ├── net/                   #   网络层 (IPv4, ICMP)
│   ├── transport/             #   传输层 (TCP, UDP, Reno, CUBIC, Orca)
│   ├── app/                   #   应用层 (HTTP, DNS)
│   ├── metrics/               #   指标采集与导出
│   └── ai/                    #   AI 智能面 (ONNX 推理)
├── src/                       # C++ 源代码实现 (~7,200 行)
├── tests/                     # C++ 测试代码 (~3,200 行)
│   ├── unit/                  #   16 个单元测试 (Catch2)
│   ├── integration/           #   集成测试 (TCP 握手, HTTP 往返)
│   └── benchmark/             #   性能基准 (校验和, 队列, TCP 吞吐)
├── training/                  # Python 训练代码 (~4,000 行)
│   ├── orca/                  #   SAC 拥塞控制训练
│   ├── bandwidth/             #   LSTM 带宽预测训练
│   ├── anomaly/               #   Autoencoder 异常检测训练
│   └── environment.yml        #   Conda 环境配置
├── models/                    # 预训练 ONNX 模型
├── scripts/                   # Shell 脚本 (~2,700 行)
│   ├── nat/                   #   NAT 设置/清理
│   ├── mac/ linux/            #   平台数据采集
│   └── *.py *.sh              #   数据预处理、ONNX Runtime 下载等
├── examples/                  # 示例程序
│   ├── minimal.cpp            #   最小示例 (26 行 HTTP 服务器)
│   └── neustack_demo.cpp      #   完整交互式 Demo
├── docs/                      # 文档
└── cmake/                     # CMake 模块 (平台检测, ONNX 查找)
```

## 测试

```bash
cd build

# 运行全部 42 个测试
ctest --output-on-failure

# 按类别运行
ctest -R "unit"         # 单元测试
ctest -R "Integration"  # 集成测试
ctest -R "Benchmark"    # 基准测试
```

| 类别 | 数量 | 覆盖 |
|------|------|------|
| 单元测试 | 16 | 校验和、IP 地址、TCP 序列号、拥塞控制 (CUBIC/Reno/Orca)、TCP 构造/解析、HTTP 解析/类型、环形缓冲区、SPSC 队列、AI 特征提取、AI Agent |
| 集成测试 | 2 | TCP 三次握手 + 数据回显、HTTP 完整请求/响应往返 |
| 基准测试 | 3 | 校验和吞吐量、SPSC 队列吞吐量、TCP 段处理吞吐量 |

## NAT 配置（本地测试）

macOS 上通过 pf 实现 NAT，让协议栈访问公网：

```bash
# 启动 NAT
sudo ./scripts/nat/setup_nat.sh --dev utun4

# 清理 NAT
sudo ./scripts/nat/teardown_nat.sh --dev utun4
```

## 许可证

MIT License
