# Firewall API Reference

The NeuStack firewall engine provides zero-allocation packet filtering with integrated AI anomaly detection.

## Table of Contents

- [Overview](#overview)
- [NeuStack Facade API](#neustack-facade-api)
- [FirewallEngine](#firewallengine)
- [RuleEngine](#ruleengine)
- [RateLimiter](#ratelimiter)
- [Rule](#rule)
- [FirewallAI](#firewallai)
- [Shadow Mode Auto-Escalation/De-escalation](#shadow-mode-auto-escalationde-escalation)
- [SecurityAnomalyModel](#securityanomalymodel)
- [SecurityExporter](#securityexporter)
- [Demo Interactive Commands](#demo-interactive-commands)
- [Usage Examples](#usage-examples)
- [Notes](#notes)

---

## Overview

### Architecture

```
┌─ Network Layer Entry ───────────────────────────────┐
│                                                │
│   recv() → FirewallEngine::inspect()           │
│               │                                │
│               ├─ Whitelist check (O(1))         │
│               ├─ Blacklist check (O(1))         │
│               ├─ Rate limit check (Token Bucket) │
│               ├─ Custom rule matching           │
│               └─ FirewallAI anomaly detection   │
│               │                                │
│               ▼                                │
│   IPv4Layer::on_receive() (when passed)        │
│                                                │
└────────────────────────────────────────────────┘
```

### Matching Priority

1. **Whitelist** - Highest priority, pass immediately
2. **Blacklist** - Drop immediately
3. **Rate limiting** - Drop when limit exceeded
4. **Custom rules** - Sorted by priority value
5. **AI anomaly detection** - Shadow Mode alert or Enforce Mode block
6. **Default pass**

---

## NeuStack Facade API

Access firewall functionality through the `NeuStack` facade class without directly operating internal engines.

### Header File

```cpp
#include "neustack/neustack.hpp"
```

### StackConfig Firewall-Related Configuration

```cpp
struct StackConfig {
    // 防火墙开关
    bool enable_firewall = true;          // 启用防火墙
    bool firewall_shadow_mode = true;     // Shadow Mode: AI 只告警不阻断

    // AI 安全模型
    std::string security_model_path;      // ONNX 模型路径（空 = 不启用 AI）
    float security_threshold = 0.0f;      // 异常阈值（0 = 从模型 metadata 读取）
};
```

> **Threshold Note**: `security_threshold = 0.0f` means the optimal threshold saved during training is automatically read from the ONNX model's metadata. Manually specifying a positive number overrides the model's built-in threshold.

### Facade Methods

```cpp
auto stack = NeuStack::create(config);

// ─── Status Queries ───
bool firewall_enabled() const;         // 防火墙是否启用
bool firewall_ai_enabled() const;      // AI 模型是否已加载
bool firewall_shadow_mode() const;     // 当前是否 Shadow Mode

// ─── Configuration ───
void firewall_set_shadow_mode(bool shadow);  // 动态切换 Shadow/Enforce Mode
void firewall_set_threshold(float threshold); // 动态调整 AI 阈值

// ─── Data Plane (for manual event loop) ───
bool firewall_inspect(const uint8_t* data, size_t len);  // 检查包，true=放行
void firewall_on_timer();                                 // 每秒调用一次

// ─── Advanced Access ───
RuleEngine* firewall_rules();          // 直接操作规则引擎（添加黑白名单、规则等）
FirewallStats firewall_stats() const;  // 防火墙统计
FirewallAIStats firewall_ai_stats() const;  // AI 统计
```

### Typical Usage

```cpp
StackConfig config;
config.enable_firewall = true;
config.firewall_shadow_mode = true;
config.security_model_path = "models/security_anomaly.onnx";
config.security_threshold = 0.0f;  // 使用模型内置阈值

auto stack = NeuStack::create(config);

// 添加规则
auto* rules = stack->firewall_rules();
rules->add_blacklist_ip(ip_from_string("1.2.3.4"));
rules->rate_limiter().set_enabled(true);
rules->rate_limiter().set_rate(1000, 100);

// 查看统计
auto stats = stack->firewall_stats();
auto ai_stats = stack->firewall_ai_stats();
printf("Inspected: %lu, Anomalies: %lu\n",
       stats.packets_inspected, ai_stats.anomalies_detected);
```

---

## FirewallEngine

The main firewall engine responsible for packet inspection.

### Header File

```cpp
#include "neustack/firewall/firewall_engine.hpp"
```

### Construction

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

### Main Methods

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

The rule management engine that manages whitelists, blacklists, custom rules, and rate limiting.

### Header File

```cpp
#include "neustack/firewall/rule_engine.hpp"
```

### Whitelist/Blacklist

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

### Custom Rules

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

### Rate Limiter Access

```cpp
RateLimiter& rate_limiter();
```

### Statistics

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

Token bucket rate limiter with per-IP PPS limits.

### Header File

```cpp
#include "neustack/firewall/rate_limiter.hpp"
```

### Configuration

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

### Manual Check (usually not needed)

```cpp
struct Result {
    bool allowed;           // 是否放行
    uint32_t current_pps;   // 当前估算 PPS
    uint32_t limit_pps;     // 限制 PPS
    uint32_t tokens_left;   // 剩余令牌
};

Result check(uint32_t ip, uint64_t now_us);
```

### Reset

```cpp
void reset(uint32_t ip);  // 重置特定 IP
void clear();             // 清空所有状态
size_t tracked_count() const;
```

---

## Rule

A single firewall rule.

### Header File

```cpp
#include "neustack/firewall/rule.hpp"
```

### Structure

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

### Factory Methods

```cpp
// IP 黑名单规则
static Rule blacklist_ip(uint16_t id, uint32_t ip, uint16_t priority = 100);

// IP 白名单规则
static Rule whitelist_ip(uint16_t id, uint32_t ip, uint16_t priority = 50);

// 端口封禁规则
static Rule block_port(uint16_t id, uint16_t port, uint8_t proto = 0, uint16_t priority = 100);
```

### Match Method

```cpp
bool matches(uint32_t src_ip, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             uint8_t protocol) const;
```

---

## FirewallAI

The firewall AI layer, integrating `SecurityAnomalyModel` for traffic anomaly detection.

### Header File

```cpp
#include "neustack/firewall/firewall_ai.hpp"
```

### Configuration

```cpp
FirewallAIConfig config;
config.model_path = "models/security_anomaly.onnx";
config.anomaly_threshold = 0.0f;        // 0 = 从模型 metadata 自动读取
config.shadow_mode = true;              // 只告警不阻断
config.inference_interval_ms = 1000;    // 每秒推理一次

// 自动升降级（默认关闭）
config.auto_escalate = false;
config.escalate_consecutive = 5;        // 连续 5 次异常 → 关闭 Shadow Mode
config.deescalate_normal_count = 30;    // 连续 30 次正常 → 恢复 Shadow Mode
config.escalate_cooldown_ms = 60000;    // 升降级后 60 秒冷静期

FirewallAI ai(config);
```

### Usage Pattern

```cpp
// 1. 数据面：每个包调用
ai.record_packet(evt);

// 2. 定时器：每秒调用
ai.tick();

// 3. 定时器：每 N 秒执行 AI 推理
float score = ai.run_inference();

// 4. 数据面：获取缓存的 AI 决策
FirewallDecision dec = ai.evaluate();
// dec.action == PASS / ALERT (Shadow) / DROP (Enforce)
```

### Configuration API

```cpp
void set_threshold(float threshold);
float threshold() const;

void set_shadow_mode(bool shadow);
bool shadow_mode() const;

void set_inference_interval_ms(uint32_t ms);
uint32_t inference_interval_ms() const;
```

### Alert Callback

```cpp
ai.set_alert_callback([](float score, const SecurityMetrics::Snapshot& snap) {
    printf("[AI ALERT] score=%.4f pps=%.1f syn_rate=%.1f\n",
           score, snap.pps, snap.syn_rate);
});
```

### FirewallAIStats

```cpp
struct FirewallAIStats {
    uint64_t inferences_total;    // 总推理次数
    uint64_t anomalies_detected;  // 检测到的异常次数
    uint64_t alerts_triggered;    // 触发告警次数
    uint64_t drops_by_ai;         // AI 导致的丢包（非 Shadow Mode）

    float last_anomaly_score;     // 最近一次异常分数
    float max_anomaly_score;      // 历史最高异常分数
    uint64_t last_anomaly_time_ms; // 最近一次异常的时间戳（毫秒）

    uint64_t escalations;         // 自动升级次数（Shadow → Enforce）
    uint64_t deescalations;       // 自动恢复次数（Enforce → Shadow）
};

const Stats& stats = ai.stats();
```

---

## Shadow Mode Auto-Escalation/De-escalation

FirewallAI supports automatic switching between Shadow Mode (alert only) and Enforce Mode (block), based on consecutive anomalous/normal inference results.

### Workflow

```
Normal Traffic ──→ Shadow Mode (alert only)
                │
          N consecutive anomalies (default 5)
                ↓
         Enforce Mode (block anomalous traffic)
                │
          M consecutive normal (default 30)
                ↓
         Shadow Mode (restore alert mode)
```

### Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `auto_escalate` | `false` | Whether to enable auto-escalation/de-escalation |
| `escalate_consecutive` | `5` | Consecutive anomaly count threshold; Shadow Mode is disabled when reached |
| `deescalate_normal_count` | `30` | Consecutive normal count threshold; Shadow Mode is restored when reached |
| `escalate_cooldown_ms` | `60000` | Cooldown period after escalation/de-escalation (ms), prevents oscillation |

### Cooldown Mechanism

A cooldown timer starts after each escalation or de-escalation (default 60 seconds). No further mode changes occur during the cooldown period, preventing frequent switching during traffic fluctuations.

### Configuration Notes

`auto_escalate` is configured in `FirewallAIConfig` and is disabled by default. The current version's `StackConfig` high-level API does not expose this parameter yet; it needs to be enabled by directly constructing `FirewallAIConfig` or manually in the demo.

---

## SecurityAnomalyModel

A Deep Autoencoder anomaly detection model dedicated to the firewall.

### Header File

```cpp
#include "neustack/ai/security_model.hpp"
```

### Comparison with Other Models

| Model | Interface | Input | Output | Purpose |
|-------|-----------|-------|--------|---------|
| Orca | `ICongestionModel` | 7-dim TCP features | α | Congestion control |
| Bandwidth Prediction | `IBandwidthModel` | Time series history | bytes/s | Bandwidth prediction |
| AnomalyDetector | `IAnomalyModel` | 8-dim general traffic | Error + anomaly | TCP anomaly detection |
| **SecurityAnomalyModel** | **`ISecurityModel`** | **8-dim security features** | **Error + anomaly + confidence** | **Firewall threat detection** |

### ISecurityModel::Input (8-dim)

```cpp
struct Input {
    float pps_norm;             // 包速率（归一化）
    float bps_norm;             // 字节速率（归一化）
    float syn_rate_norm;        // SYN 速率（归一化）
    float rst_rate_norm;        // RST 速率（归一化）
    float syn_ratio_norm;       // SYN 占比 (syn_rate/pps)
    float new_conn_rate_norm;   // 新连接速率（归一化）
    float avg_pkt_size_norm;    // 平均包大小（归一化）
    float rst_ratio_norm;       // RST/总包 比率（归一化）
};
```

Normalization method: `value / max_value`, where `max_value` is defined in `SecurityFeatureExtractor` (e.g., `max_pps = 20000`). The Python training side and C++ inference side use the same normalization parameters.

### ISecurityModel::Output

```cpp
struct Output {
    float reconstruction_error; // MSE 重构误差
    bool is_anomaly;            // error > threshold
    float confidence;           // [0, 1] 置信度 (sigmoid 映射)
};
```

### Confidence Calculation

Confidence is based on the distance between the error and the threshold, using a sigmoid mapping:

```
confidence = 1 / (1 + exp(-6 * (error/threshold - 1)))
```

- `error << threshold` → confidence ≈ 0 (confident normal)
- `error == threshold` → confidence ≈ 0.5
- `error >> threshold` → confidence ≈ 1.0 (confident anomaly)

### Usage Example

```cpp
SecurityAnomalyModel model("models/security_anomaly.onnx");

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

auto result = model.infer(input);
// result->reconstruction_error  重构误差
// result->is_anomaly            是否异常
// result->confidence            置信度 [0, 1]
```

---

## SecurityExporter

A CSV data exporter for collecting data for security model training.

### Header File

```cpp
#include "neustack/metrics/security_exporter.hpp"
```

### Usage

```cpp
SecurityMetrics metrics;
SecurityExporter exporter("security_data.csv", metrics);

// 每秒采样一次
exporter.flush(0);   // label=0 正常
exporter.flush(1);   // label=1 异常（手动标注）
exporter.sync();     // 强制刷盘
```

### CSV Columns

```
timestamp_ms, packets_total, bytes_total,
syn_packets, syn_ack_packets, rst_packets,
pps, bps, syn_rate, rst_rate,
syn_ratio, new_conn_rate, avg_pkt_size, rst_ratio,
label
```

Raw values and derived rates are exported together; normalization is handled by the Python training side. `label`: `0`=normal, `1`=anomaly.

---

## Demo Interactive Commands

`neustack_demo` provides real-time firewall management commands:

| Command | Description |
|---------|-------------|
| `f` | Display real-time firewall statistics (including AI inference count, anomaly score, escalation/de-escalation counts) |
| `fw shadow on` | Enable Shadow Mode (alert only, no blocking) |
| `fw shadow off` | Disable Shadow Mode (anomalous traffic will be blocked) |
| `fw threshold <val>` | Adjust AI anomaly threshold |
| `fw bl add <ip>` | Add IP to blacklist |
| `fw bl del <ip>` | Remove IP from blacklist |
| `m` | View global protocol stack metrics |

### HTTP API

| Endpoint | Description |
|----------|-------------|
| `GET /api/status` | Protocol stack running status |
| `GET /api/info` | List of enabled services (including firewall, firewall-ai) |
| `GET /api/firewall/status` | Complete firewall status JSON (including rule statistics, AI statistics, Shadow Mode status) |

---

## Usage Examples

### Basic Configuration

```cpp
#include "neustack/neustack.hpp"

auto stack = NeuStack::create();
auto* rules = stack->firewall_rules();

// 白名单本地网络
rules->add_whitelist_ip(ip_from_string("192.168.1.0") & 0xFFFFFF00);

// 黑名单已知攻击 IP
rules->add_blacklist_ip(ip_from_string("1.2.3.4"));

// 封禁危险端口
rules->add_rule(Rule::block_port(1, 22, 6));   // SSH
rules->add_rule(Rule::block_port(2, 23, 6));   // Telnet
rules->add_rule(Rule::block_port(3, 3389, 6)); // RDP
```

### DDoS Protection

```cpp
// 启用限速
auto& limiter = rules->rate_limiter();
limiter.set_enabled(true);
limiter.set_rate(1000, 100);  // 1000 PPS per IP, 突发 100
```

### Monitoring Statistics

```cpp
void print_stats(NeuStack& stack) {
    auto stats = stack.firewall_stats();
    printf("Inspected: %lu, Passed: %lu, Dropped: %lu, Alerted: %lu\n",
           stats.packets_inspected,
           stats.packets_passed,
           stats.packets_dropped,
           stats.packets_alerted);

    auto ai_stats = stack.firewall_ai_stats();
    printf("AI Inferences: %lu, Anomalies: %lu, Score: %.4f\n",
           ai_stats.inferences_total,
           ai_stats.anomalies_detected,
           ai_stats.last_anomaly_score);
}
```

### Decision Callback

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

## Notes

1. **IP byte order**: Methods like `add_blacklist_ip()` accept IP addresses in **network byte order**
2. **Thread safety**: FirewallEngine is not thread-safe and should be used in a single thread; FirewallAI's `evaluate()` can be called from any thread (reads atomic cache)
3. **Memory management**: Uses FixedPool, zero allocation on hot paths
4. **Performance**: Whitelist/blacklist O(1), custom rules O(n)
5. **AI inference frequency**: Default once per second (`inference_interval_ms = 1000`); results are cached for data plane use without blocking the hot path
6. **Threshold default**: `anomaly_threshold = 0.0f` means the optimal threshold determined during training is automatically read from ONNX model metadata
