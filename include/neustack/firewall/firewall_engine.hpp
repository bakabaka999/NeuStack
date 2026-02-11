#ifndef NEUSTACK_FIREWALL_FIREWALL_ENGINE_HPP
#define NEUSTACK_FIREWALL_FIREWALL_ENGINE_HPP

#include "neustack/firewall/packet_event.hpp"
#include "neustack/firewall/firewall_decision.hpp"
#include "neustack/common/memory_pool.hpp"

#include <cstdint>
#include <functional>

namespace neustack {

/**
 * 防火墙配置
 */
struct FirewallConfig {
    bool enabled = true;              // 防火墙总开关
    bool shadow_mode = true;          // Shadow Mode: AI 检测只告警不阻断
    bool log_passed = false;          // 是否记录放行的包
    bool log_dropped = true;          // 是否记录丢弃的包

    size_t event_pool_size = 4096;    // PacketEvent 池大小
    size_t decision_pool_size = 4096; // FirewallDecision 池大小
};

/**
 * 防火墙统计
 */
struct FirewallStats {
    uint64_t packets_inspected = 0;   // 检查的包总数
    uint64_t packets_passed = 0;      // 放行的包
    uint64_t packets_dropped = 0;     // 丢弃的包
    uint64_t packets_alerted = 0;     // 告警的包 (Shadow Mode)

    uint64_t pool_acquire_failed = 0; // 池耗尽次数
};

// 前向声明
struct IPv4Packet;

/**
 * 防火墙引擎 - 数据包检测的核心类
 *
 * 职责:
 * 1. 解析 IPv4 包为 PacketEvent
 * 2. 执行规则检查 (Phase 2)
 * 3. AI 异常检测 (Phase 3)
 * 4. 返回 FirewallDecision
 *
 * 线程模型:
 * - 主线程调用 inspect()，同步返回
 * - AI 推理在 IntelligencePlane 线程异步执行
 *
 * 内存管理:
 * - 使用 FixedPool 池化 PacketEvent 和 FirewallDecision
 * - 零动态分配
 */
class FirewallEngine {
public:
    // 池大小编译期常量
    static constexpr size_t EVENT_POOL_SIZE = 4096;
    static constexpr size_t DECISION_POOL_SIZE = 4096;

    // 决策回调类型 (用于日志/调试)
    using DecisionCallback = std::function<void(const PacketEvent&, const FirewallDecision&)>;

    // ─── 构造与配置 ───

    explicit FirewallEngine(const FirewallConfig& config = {});
    ~FirewallEngine() = default;

    // 禁止拷贝
    FirewallEngine(const FirewallEngine&) = delete;
    FirewallEngine& operator=(const FirewallEngine&) = delete;

    // ─── 核心 API ───

    /**
     * @brief 检查数据包是否应该放行
     *
     * @param data 原始 IP 包数据
     * @param len 数据长度
     * @return true = 放行, false = 丢弃
     *
     * 这是热路径，必须极快返回。
     */
    bool inspect(const uint8_t* data, size_t len);

    /**
     * @brief 使用已解析的 IPv4Packet 检查
     *
     * 当上层已经解析过 IPv4 头时，避免重复解析。
     */
    bool inspect(const IPv4Packet& pkt);

    // ─── 配置 API ───

    void set_enabled(bool enabled) { _config.enabled = enabled; }
    bool enabled() const { return _config.enabled; }

    void set_shadow_mode(bool shadow) { _config.shadow_mode = shadow; }
    bool shadow_mode() const { return _config.shadow_mode; }

    // ─── 回调注册 ───

    void set_decision_callback(DecisionCallback cb) { _on_decision = std::move(cb); }

    // ─── 统计 API ───

    const FirewallStats& stats() const { return _stats; }
    void reset_stats() { _stats = {}; }

    // ─── 调试 API ───

    size_t event_pool_available() const { return _event_pool.available(); }
    size_t decision_pool_available() const { return _decision_pool.available(); }

private:
    // ─── 内部方法 ───

    // 从原始数据解析 PacketEvent
    bool parse_packet(const uint8_t* data, size_t len, PacketEvent* evt);

    // 从 IPv4Packet 填充 PacketEvent
    void fill_event_from_ipv4(const IPv4Packet& pkt, PacketEvent* evt);

    // 执行检查逻辑 (当前只有默认 PASS，后续 Phase 加规则)
    FirewallDecision evaluate(const PacketEvent& evt);

    // 记录决策 (日志/统计)
    void record_decision(const PacketEvent& evt, const FirewallDecision& decision);

    // 获取当前时间戳 (微秒)
    uint64_t now_us() const;

private:
    // ─── 配置 ───
    FirewallConfig _config;

    // ─── 内存池 ───
    FixedPool<PacketEvent, EVENT_POOL_SIZE> _event_pool;
    FixedPool<FirewallDecision, DECISION_POOL_SIZE> _decision_pool;

    // ─── 统计 ───
    FirewallStats _stats;

    // ─── 回调 ───
    DecisionCallback _on_decision;
};

} // namespace neustack

#endif // NEUSTACK_FIREWALL_FIREWALL_ENGINE_HPP
