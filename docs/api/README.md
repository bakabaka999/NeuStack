# NeuStack API Reference

本目录包含 NeuStack 的 API 参考文档。

## 目录

| 文档 | 说明 |
|------|------|
| [Firewall](firewall.md) | 防火墙引擎 API |
| [AI Training](ai-training.md) | AI 模型训练指南 |
| [NeuStack Core](core.md) | 核心 API (协议栈/HTTP/DNS) |

## 快速链接

### 防火墙

```cpp
#include "neustack/firewall.hpp"

// 主要类
FirewallEngine      // 防火墙主引擎
RuleEngine          // 规则管理
RateLimiter         // 令牌桶限速
Rule                // 单条规则
PacketEvent         // 解析后的数据包
FirewallDecision    // 防火墙决策
```

### 协议栈

```cpp
#include "neustack/neustack.hpp"

// 主要类
NeuStack            // 协议栈主类
HttpServer          // HTTP 服务器
HttpClient          // HTTP 客户端
DNSClient           // DNS 客户端
TCPLayer            // TCP 层
IPv4Layer           // IP 层
```

### AI 智能面

```cpp
#include "neustack/ai/network_agent.hpp"

// 主要类
NetworkAgent        // AI 决策层
OrcaModel           // SAC 拥塞控制
BandwidthPredictor  // LSTM 带宽预测
AnomalyDetector     // Autoencoder 异常检测
```

## 版本

- **v1.0**: 基础协议栈 + AI 拥塞控制
- **v1.1**: 性能优化 + 测试加固
- **v1.2**: AI 防火墙 + 安全训练管线 (当前)
