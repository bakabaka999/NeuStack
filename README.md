<p align="center">
  <h1 align="center">NeuStack</h1>
  <p align="center">
    <strong>从零实现的跨平台用户态 TCP/IP 协议栈，集成 AI 拥塞控制与智能防火墙</strong>
  </p>
  <p align="center">
    <a href="https://github.com/bakabaka999/NeuStack/actions"><img src="https://github.com/bakabaka999/NeuStack/workflows/CI/badge.svg" alt="CI"></a>
    <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg" alt="C++20"></a>
    <a href="https://pytorch.org/"><img src="https://img.shields.io/badge/PyTorch-SAC%20%2F%20LSTM%20%2F%20AE-EE4C2C.svg" alt="PyTorch"></a>
    <a href="https://onnxruntime.ai/"><img src="https://img.shields.io/badge/ONNX%20Runtime-inference-7B68EE.svg" alt="ONNX Runtime"></a>
    <img src="https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey.svg" alt="Platform">
    <img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License">
  </p>
</p>

---

## 📖 目录

- [项目简介](#项目简介)
- [核心特性](#核心特性)
- [系统架构](#系统架构)
- [快速开始](#快速开始)
- [文档](#文档)
- [AI 智能面](#ai-智能面--networkagent)
- [防火墙](#防火墙)
- [测试](#测试)
- [项目结构](#项目结构)
- [许可证](#许可证)

---

## 项目简介

NeuStack 是一个**完全从零实现**的用户态 TCP/IP 协议栈，具备 AI 驱动的拥塞控制和智能防火墙能力。

- **C++ 核心**：~10,000 行，实现完整协议栈 + AI 推理 + 防火墙
- **Python 训练**：~4,000 行，SAC/LSTM/Autoencoder 模型训练
- **Shell 脚本**：~2,700 行，数据采集与环境配置

## 核心特性

| 类别 | 内容 |
|------|------|
| **协议栈** | IPv4 / ICMP / UDP / TCP / HTTP 1.1 / DNS |
| **拥塞控制** | Reno · CUBIC · **Orca (SAC 强化学习)** |
| **AI 智能面** | 带宽预测 (LSTM) · 异常检测 (Autoencoder) · 智能拥塞控制 (SAC) · 安全异常检测 (Autoencoder) |
| **防火墙** | 黑白名单 · 端口封禁 · 令牌桶限速 · AI Shadow Mode · 安全数据导出 |
| **NetworkAgent** | 4 状态决策层，协调 4 个模型，策略性 clamp / 回退 / 连接控制 |
| **跨平台** | macOS (utun) · Linux (TUN/TAP) · Windows (Wintun) |
| **零分配设计** | FixedPool 内存池，热路径无 new/delete |

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                    应用层 (Application)                       │
│              HTTP Server/Client  ·  DNS Client                │
├─────────────────────────────────────────────────────────────┤
│                    传输层 (Transport)                         │
│          TCP (Reno / CUBIC / Orca)  ·  UDP                    │
├─────────────────────────────────────────────────────────────┤
│                    网络层 (Network)                           │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              FirewallEngine                          │    │
│  │  ┌───────────┬──────────────┬───────────────────┐   │    │
│  │  │ Whitelist │  Blacklist   │  RateLimiter      │   │    │
│  │  │   (O(1))  │    (O(1))    │  (Token Bucket)   │   │    │
│  │  └───────────┴──────────────┴───────────────────┘   │    │
│  └─────────────────────────────────────────────────────┘    │
│                     IPv4  ·  ICMP                            │
├─────────────────────────────────────────────────────────────┤
│                   硬件抽象层 (HAL)                             │
│           macOS utun  ·  Linux TUN  ·  Wintun                 │
└─────────────────────────────────────────────────────────────┘
                             ↕
              ┌──────────────────────────────────┐
              │     NetworkAgent (AI 决策层)      │
              │  Orca (SAC) · 带宽预测 · 异常检测   │
              └──────────────────────────────────┘
```

## 快速开始

### 依赖

- CMake >= 3.20
- C++20 编译器 (Clang >= 14 / GCC >= 11 / MSVC 2019+)
- 可选：ONNX Runtime (AI 推理)、Catch2 v3 (测试)

### 构建

```bash
# 克隆
git clone https://github.com/bakabaka999/NeuStack.git
cd NeuStack

# 构建
cmake -B build -G Ninja
cmake --build build

# 测试
cd build && ctest --output-on-failure

# 运行 (需要 root)
sudo ./build/examples/neustack_demo
```

### 启用 AI

```bash
./scripts/download_onnxruntime.sh
cmake -B build -DNEUSTACK_ENABLE_AI=ON
cmake --build build
```

### 最小示例

```cpp
#include "neustack/neustack.hpp"

int main() {
    auto stack = neustack::NeuStack::create();

    // HTTP 服务器
    stack->http_server().get("/", [](const auto&) {
        return neustack::HttpResponse()
            .content_type("text/plain")
            .set_body("Hello from NeuStack!\n");
    });
    stack->http_server().listen(80);

    // 配置防火墙
    auto& fw = stack->firewall();
    fw.rule_engine().add_blacklist_ip(0x01020304);  // 封禁 1.2.3.4
    fw.rule_engine().rate_limiter().set_enabled(true);
    fw.rule_engine().rate_limiter().set_rate(1000, 100);  // 1000 PPS

    stack->run();
}
```

## 文档

| 文档 | 说明 |
|------|------|
| [API Reference](docs/api/README.md) | C++ API 参考文档 |
| [Firewall Guide](docs/api/firewall.md) | 防火墙配置指南 |
| [AI Training](docs/api/ai-training.md) | AI 模型训练流程 |
| [Project Whitepaper](docs/project_whitepaper.md) | 项目白皮书 (设计细节) |

## AI 智能面 & NetworkAgent

NeuStack 的 AI 子系统通过 **NetworkAgent** 协调三个模型：

| 模型 | 算法 | 用途 |
|------|------|------|
| **Orca** | SAC (强化学习) | 拥塞窗口智能调节 |
| **带宽预测** | LSTM | 前瞻性带宽估计 |
| **异常检测** | Autoencoder | 检测攻击/异常流量 |

### NetworkAgent 状态机

```
NORMAL ──(带宽骤降)──→ DEGRADED
   │                       │
   │(异常检测)         (异常检测)
   ↓                       ↓
UNDER_ATTACK ──(恢复)──→ RECOVERY ──→ NORMAL
```

详见 [AI Training Guide](docs/api/ai-training.md)。

## 防火墙

NeuStack 内置零分配防火墙引擎，支持：

- **黑白名单**：O(1) 哈希查找
- **端口封禁**：支持协议过滤 (TCP/UDP)
- **令牌桶限速**：per-IP PPS 限制
- **Shadow Mode**：AI 检测只告警不阻断

```cpp
auto& rules = stack->firewall().rule_engine();

// 白名单 (最高优先级)
rules.add_whitelist_ip(ip_from_string("192.168.1.1"));

// 黑名单
rules.add_blacklist_ip(ip_from_string("1.2.3.4"));

// 封禁端口
rules.add_rule(Rule::block_port(1, 22, 6));  // 封 SSH (TCP)

// 限速
rules.rate_limiter().set_enabled(true);
rules.rate_limiter().set_rate(1000, 100);  // 1000 PPS, 突发 100
```

详见 [Firewall Guide](docs/api/firewall.md)。

## 测试

```bash
cd build
ctest --output-on-failure

# 按类别
ctest -R "unit"         # 单元测试
ctest -R "Integration"  # 集成测试
ctest -R "Benchmark"    # 基准测试
```

| 类别 | 覆盖 |
|------|------|
| **单元测试** | 校验和、TCP/IP 解析、拥塞控制、HTTP 解析、防火墙规则引擎、限速器、安全模型 |
| **集成测试** | TCP 握手、HTTP 往返、防火墙数据包过滤、AI Shadow Mode |
| **基准测试** | 校验和吞吐、队列性能、TCP 吞吐、内存池性能 |

## 项目结构

```
NeuStack/
├── include/neustack/          # 公共头文件
│   ├── common/                #   通用工具
│   ├── hal/                   #   硬件抽象层
│   ├── net/                   #   网络层 (IPv4, ICMP)
│   ├── transport/             #   传输层 (TCP, UDP)
│   ├── app/                   #   应用层 (HTTP, DNS)
│   ├── firewall/              #   防火墙引擎
│   ├── metrics/               #   指标采集
│   └── ai/                    #   AI 推理
├── src/                       # 源代码实现
├── tests/                     # 测试代码
├── training/                  # Python 训练代码
├── models/                    # ONNX 模型
├── scripts/                   # Shell 脚本
├── examples/                  # 示例程序
├── docs/                      # 文档
│   ├── api/                   #   API 参考
│   └── project_whitepaper.md  #   白皮书
└── cmake/                     # CMake 模块
```

## 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `NEUSTACK_BUILD_TESTS` | ON | 编译测试 |
| `NEUSTACK_BUILD_EXAMPLES` | ON | 编译示例 |
| `NEUSTACK_ENABLE_ASAN` | OFF | Address Sanitizer |
| `NEUSTACK_ENABLE_AI` | OFF | AI 拥塞控制 |

## 许可证

[MIT License](LICENSE)

---

<p align="center">
  <sub>Made with ❤️ by <a href="https://github.com/bakabaka999">bakabaka999</a></sub>
</p>
