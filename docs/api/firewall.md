# Firewall API Reference

NeuStack 防火墙引擎提供零分配的数据包过滤能力。

## 目录

- [概述](#概述)
- [FirewallEngine](#firewallengine)
- [RuleEngine](#ruleengine)
- [RateLimiter](#ratelimiter)
- [Rule](#rule)
- [使用示例](#使用示例)

---

## 概述

### 架构

```
┌─ 网络层入口 ────────────────────────────────────┐
│                                                │
│   recv() → FirewallEngine::inspect()           │
│               │                                │
│               ├─ 白名单检查 (O(1))              │
│               ├─ 黑名单检查 (O(1))              │
│               ├─ 限速检查 (Token Bucket)        │
│               ├─ 自定义规则匹配                 │
│               └─ AI Shadow Mode (Phase 3)      │
│               │                                │
│               ▼                                │
│   IPv4Layer::on_receive() (放行时)             │
│                                                │
└────────────────────────────────────────────────┘
```

### 匹配优先级

1. **白名单** - 最高优先级，直接放行
2. **黑名单** - 直接丢弃
3. **限速** - 超限丢弃
4. **自定义规则** - 按 priority 值排序
5. **默认放行**

---

## FirewallEngine

防火墙主引擎，负责数据包检查。

### 头文件

```cpp
#include "neustack/firewall/firewall_engine.hpp"
```

### 构造

```cpp
// 使用默认配置
FirewallEngine fw;

// 自定义配置
FirewallConfig config;
config.enabled = true;
config.shadow_mode = true;  // AI 检测只告警
config.log_dropped = true;
FirewallEngine fw(config);
```

### 主要方法

```cpp
// 检查数据包
bool inspect(const uint8_t* data, size_t len);
bool inspect(const IPv4Packet& pkt);

// 启用/禁用
void set_enabled(bool enabled);
bool enabled() const;

// Shadow Mode (AI 检测只告警不阻断)
void set_shadow_mode(bool shadow);
bool shadow_mode() const;

// 获取规则引擎
RuleEngine& rule_engine();

// 统计
const FirewallStats& stats() const;
void reset_stats();
```

### FirewallStats

```cpp
struct FirewallStats {
    uint64_t packets_inspected;   // 检查的包总数
    uint64_t packets_passed;      // 放行的包
    uint64_t packets_dropped;     // 丢弃的包
    uint64_t packets_alerted;     // 告警的包 (Shadow Mode)
    uint64_t pool_acquire_failed; // 池耗尽次数
};
```

---

## RuleEngine

规则管理引擎，管理黑白名单、自定义规则和限速。

### 头文件

```cpp
#include "neustack/firewall/rule_engine.hpp"
```

### 黑白名单

```cpp
RuleEngine& rules = fw.rule_engine();

// 白名单 (优先级最高)
rules.add_whitelist_ip(ip);
rules.remove_whitelist_ip(ip);
rules.clear_whitelist();
size_t whitelist_size() const;

// 黑名单
rules.add_blacklist_ip(ip);
rules.remove_blacklist_ip(ip);
rules.clear_blacklist();
size_t blacklist_size() const;
```

### 自定义规则

```cpp
// 添加规则
bool add_rule(const Rule& rule);

// 移除规则
bool remove_rule(uint16_t rule_id);

// 启用/禁用规则
bool set_rule_enabled(uint16_t rule_id, bool enabled);

// 清空所有规则
void clear_rules();

// 规则数量
size_t rule_count() const;
```

### 限速器访问

```cpp
RateLimiter& rate_limiter();
```

### 统计

```cpp
struct Stats {
    uint64_t whitelist_hits;
    uint64_t blacklist_hits;
    uint64_t rate_limit_drops;
    uint64_t rule_matches;
    uint64_t default_passes;
};

const Stats& stats() const;
void reset_stats();
```

---

## RateLimiter

令牌桶限速器，per-IP PPS 限制。

### 头文件

```cpp
#include "neustack/firewall/rate_limiter.hpp"
```

### 配置

```cpp
RateLimiter& limiter = rules.rate_limiter();

// 启用/禁用
limiter.set_enabled(true);
bool enabled() const;

// 设置速率
// pps: 每秒允许的包数
// burst: 突发容量 (桶大小)
limiter.set_rate(1000, 100);  // 1000 PPS, 突发 100
```

### 手动检查 (通常不需要)

```cpp
struct Result {
    bool allowed;           // 是否放行
    uint32_t current_pps;   // 当前估算 PPS
    uint32_t limit_pps;     // 限制 PPS
    uint32_t tokens_left;   // 剩余令牌
};

Result check(uint32_t ip, uint64_t now_us);
```

### 重置

```cpp
void reset(uint32_t ip);  // 重置特定 IP
void clear();             // 清空所有状态
size_t tracked_count() const;
```

---

## Rule

单条防火墙规则。

### 头文件

```cpp
#include "neustack/firewall/rule.hpp"
```

### 结构

```cpp
struct Rule {
    // 匹配条件 (0 = any)
    uint32_t src_ip;
    uint32_t src_mask;
    uint32_t dst_ip;
    uint32_t dst_mask;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;      // 0=any, 6=TCP, 17=UDP
    
    // 规则属性
    uint16_t id;
    uint16_t priority;      // 越小越优先
    FirewallDecision::Action action;
    FirewallDecision::Reason reason;
    bool enabled;
};
```

### 工厂方法

```cpp
// IP 黑名单规则
static Rule blacklist_ip(uint16_t id, uint32_t ip, uint16_t priority = 100);

// IP 白名单规则
static Rule whitelist_ip(uint16_t id, uint32_t ip, uint16_t priority = 50);

// 端口封禁规则
static Rule block_port(uint16_t id, uint16_t port, uint8_t proto = 0, uint16_t priority = 100);
```

### 匹配方法

```cpp
bool matches(uint32_t src_ip, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             uint8_t protocol) const;
```

---

## 使用示例

### 基础配置

```cpp
#include "neustack/neustack.hpp"

auto stack = NeuStack::create();
auto& fw = stack->firewall();
auto& rules = fw.rule_engine();

// 白名单本地网络
rules.add_whitelist_ip(ip_from_string("192.168.1.0") & 0xFFFFFF00);

// 黑名单已知攻击 IP
rules.add_blacklist_ip(ip_from_string("1.2.3.4"));

// 封禁危险端口
rules.add_rule(Rule::block_port(1, 22, 6));   // SSH
rules.add_rule(Rule::block_port(2, 23, 6));   // Telnet
rules.add_rule(Rule::block_port(3, 3389, 6)); // RDP
```

### DDoS 防护

```cpp
// 启用限速
auto& limiter = rules.rate_limiter();
limiter.set_enabled(true);
limiter.set_rate(1000, 100);  // 1000 PPS per IP, 突发 100
```

### 监控统计

```cpp
// 定期打印统计
void print_stats() {
    auto& stats = fw.stats();
    printf("Inspected: %lu, Passed: %lu, Dropped: %lu\n",
           stats.packets_inspected,
           stats.packets_passed,
           stats.packets_dropped);
    
    auto& rule_stats = rules.stats();
    printf("Whitelist hits: %lu, Blacklist hits: %lu, Rate limited: %lu\n",
           rule_stats.whitelist_hits,
           rule_stats.blacklist_hits,
           rule_stats.rate_limit_drops);
}
```

### 决策回调

```cpp
// 设置决策回调 (用于日志/监控)
fw.set_decision_callback([](const PacketEvent& evt, const FirewallDecision& dec) {
    if (dec.should_drop()) {
        printf("[DROPPED] src=%08x dst=%08x reason=%d\n",
               evt.src_ip, evt.dst_ip, static_cast<int>(dec.reason));
    }
});
```

---

## FirewallAI

防火墙 AI 层，集成 `SecurityAnomalyModel` 进行流量异常检测。

### 头文件

```cpp
#include "neustack/firewall/firewall_ai.hpp"
```

### 构造与配置

```cpp
FirewallAIConfig config;
config.model_path = "models/security_anomaly.onnx";
config.anomaly_threshold = 0.5f;
config.shadow_mode = true;          // 只告警不阻断
config.inference_interval_ms = 1000; // 每秒推理一次

FirewallAI ai(config);
```

### 使用模式

```cpp
// 1. 数据面：每个包调用
ai.record_packet(evt);

// 2. 定时器：每秒调用
ai.tick();

// 3. 定时器：每 N 秒执行 AI 推理
float score = ai.run_inference();

// 4. 数据面：获取缓存的 AI 决策
FirewallDecision dec = ai.evaluate();
```

### 告警回调

```cpp
ai.set_alert_callback([](float score, const SecurityMetrics::Snapshot& snap) {
    printf("[AI ALERT] score=%.4f pps=%.1f syn_rate=%.1f\n",
           score, snap.pps, snap.syn_rate);
});
```

### 统计

```cpp
auto& stats = ai.stats();
printf("Inferences: %lu, Anomalies: %lu, Alerts: %lu\n",
       stats.inferences_total,
       stats.anomalies_detected,
       stats.alerts_triggered);
```

---

## SecurityAnomalyModel

专用于防火墙的 Autoencoder 异常检测模型（第 4 个 AI 模型）。

### 头文件

```cpp
#include "neustack/ai/security_model.hpp"
```

### 与其他模型的对比

| 模型 | 接口 | 输入 | 输出 | 用途 |
|------|------|------|------|------|
| Orca | `ICongestionModel` | 7 维 TCP 特征 | α | 拥塞控制 |
| 带宽预测 | `IBandwidthModel` | 时序历史 | bytes/s | 带宽预测 |
| AnomalyDetector | `IAnomalyModel` | 8 维通用流量 | 误差+异常 | TCP 异常检测 |
| **SecurityAnomalyModel** | **`ISecurityModel`** | **8 维安全特征** | **误差+异常+置信度** | **防火墙威胁检测** |

### ISecurityModel::Input (8 维)

```cpp
struct Input {
    float pps_norm;             // 包速率
    float bps_norm;             // 字节速率
    float syn_rate_norm;        // SYN 速率
    float rst_rate_norm;        // RST 速率
    float syn_ratio_norm;       // SYN/SYN-ACK 比率
    float new_conn_rate_norm;   // 新连接速率
    float avg_pkt_size_norm;    // 平均包大小
    float rst_ratio_norm;       // RST/总包 比率
};
```

### ISecurityModel::Output

```cpp
struct Output {
    float reconstruction_error; // MSE 重构误差
    bool is_anomaly;            // error > threshold
    float confidence;           // [0, 1] 置信度 (sigmoid 映射)
};
```

### 置信度计算

置信度基于误差与阈值的距离，使用 sigmoid 映射：

```
confidence = 1 / (1 + exp(-6 * (error/threshold - 1)))
```

- `error << threshold` → confidence ≈ 0（确信正常）
- `error == threshold` → confidence ≈ 0.5
- `error >> threshold` → confidence ≈ 1.0（确信异常）

---

## SecurityExporter

CSV 数据导出器，为安全模型训练采集数据。

### 头文件

```cpp
#include "neustack/metrics/security_exporter.hpp"
```

### 使用

```cpp
SecurityExporter exporter("security_data.csv", metrics);

// 定时器中每秒调用
exporter.flush(0);  // label=0 正常
exporter.flush(1);  // label=1 异常 (手动标注)

// 强制刷盘
exporter.sync();
```

### CSV 列

```
timestamp_ms, packets_total, bytes_total,
syn_packets, syn_ack_packets, rst_packets,
pps, bps, syn_rate, rst_rate,
syn_ratio, new_conn_rate, avg_pkt_size, rst_ratio,
label
```

- 记录原始值 + 衍生速率，归一化由 Python 训练端负责
- `label`: `0`=正常, `1`=异常

---

## 注意事项

1. **IP 字节序**：`add_blacklist_ip()` 等方法接受**网络字节序**的 IP 地址
2. **线程安全**：FirewallEngine 不是线程安全的，应在单线程中使用
3. **内存管理**：使用 FixedPool，热路径零分配
4. **性能**：黑白名单 O(1)，自定义规则 O(n)

---

## FirewallAI

AI 驱动的异常检测层，以 Shadow Mode 或主动拦截模式运行。

### 头文件

```cpp
#include "neustack/firewall/firewall_ai.hpp"
```

### 配置

```cpp
FirewallAIConfig config;
config.model_path = "models/security_anomaly.onnx";
config.anomaly_threshold = 0.5f;    // 重构误差阈值
config.shadow_mode = true;          // 只告警不阻断
config.inference_interval_ms = 1000; // 推理频率

FirewallAI ai(config);
```

### 使用模式

```cpp
// 1. 数据面：每个包调用
ai.record_packet(evt);

// 2. 定时器：每秒调用
ai.tick();

// 3. 定时器：每 N 秒执行推理
float score = ai.run_inference();

// 4. 数据面：获取缓存的 AI 决策
auto decision = ai.evaluate();
```

### 告警回调

```cpp
ai.set_alert_callback([](float score, const SecurityMetrics::Snapshot& snap) {
    printf("[AI ALERT] score=%.4f pps=%.1f syn_rate=%.1f\n",
           score, snap.pps, snap.syn_rate);
});
```

### SecurityAnomalyModel

专用于防火墙的 Autoencoder 异常检测模型（区别于 TCP 侧的通用 `AnomalyDetector`）。

```cpp
#include "neustack/ai/security_model.hpp"

// 8 维安全特征输入
ISecurityModel::Input input{
    .pps_norm           = 0.3f,
    .bps_norm           = 0.2f,
    .syn_rate_norm      = 0.8f,   // 高 SYN → 可能是 SYN Flood
    .rst_rate_norm      = 0.1f,
    .syn_ratio_norm     = 0.9f,
    .new_conn_rate_norm = 0.7f,
    .avg_pkt_size_norm  = 0.04f,  // 小包 → 可能是攻击
    .rst_ratio_norm     = 0.05f,
};

SecurityAnomalyModel model("models/security_anomaly.onnx");
auto result = model.infer(input);
// result->reconstruction_error  重构误差
// result->is_anomaly            是否异常
// result->confidence            置信度 [0, 1]
```

---

## SecurityExporter

安全指标 CSV 导出器，为 AI 模型训练采集数据。

### 头文件

```cpp
#include "neustack/metrics/security_exporter.hpp"
```

### 使用

```cpp
SecurityMetrics metrics;
SecurityExporter exporter("security_data.csv", metrics);

// 每秒采样一次
exporter.flush(0);   // label=0 正常
exporter.flush(1);   // label=1 异常（手动标注）
exporter.sync();     // 强制刷盘
```

### CSV 列

```
timestamp_ms, packets_total, bytes_total,
syn_packets, syn_ack_packets, rst_packets,
pps, bps, syn_rate, rst_rate,
syn_ratio, new_conn_rate, avg_pkt_size, rst_ratio,
label
```

原始值 + 衍生速率一并导出，归一化由 Python 训练端负责。

---

## 版本历史

- **v1.2.0**: 初始版本，支持黑白名单、端口封禁、令牌桶限速
- **v1.2.1**: AI Shadow Mode 集成，RateLimiter O(1) LRU 升级
- **v1.2.2**: 专用 SecurityAnomalyModel + SecurityExporter 数据采集
