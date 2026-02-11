#ifndef NEUSTACK_FIREWALL_FIREWALL_DECISION_HPP
#define NEUSTACK_FIREWALL_FIREWALL_DECISION_HPP

#include <cstdint>

namespace neustack {

/**
 * 防火墙决策 - inspect() 的输出
 *
 * 设计原则:
 * - 无动态分配: 使用 enum + 数值，不用 std::string
 * - 池化友好: 固定 16 字节
 * - 可追溯: 记录触发原因和相关参数
 */
struct FirewallDecision {
    // ─── 动作类型 ───
    enum class Action : uint8_t {
        PASS  = 0,      // 放行
        DROP  = 1,      // 丢弃
        ALERT = 2,      // 告警但放行 (Shadow Mode)
        LOG   = 3,      // 仅记录
    };

    // ─── 触发原因 ───
    enum class Reason : uint8_t {
        NONE           = 0,   // 默认放行
        RULE_WHITELIST = 1,   // 白名单命中
        RULE_BLACKLIST = 2,   // 黑名单命中
        RULE_PORT      = 3,   // 端口规则命中
        RATE_LIMIT     = 4,   // 速率限制触发
        AI_ANOMALY     = 5,   // AI 异常检测
        MALFORMED      = 6,   // 畸形包
    };

    // ─── 决策字段 (16 bytes total) ───
    Action   action;          // 1 byte
    Reason   reason;          // 1 byte
    uint16_t rule_id;         // 命中的规则 ID (0 = N/A)
    float    score;           // AI 置信度 / 异常分数 [0, 1]
    uint32_t rate_current;    // 当前速率 (用于限速场景)
    uint32_t rate_limit;      // 速率上限 (用于限速场景)

    // ─── 工厂方法 ───

    static FirewallDecision pass() {
        return { Action::PASS, Reason::NONE, 0, 0.0f, 0, 0 };
    }

    static FirewallDecision drop(Reason reason, uint16_t rule_id = 0) {
        return { Action::DROP, reason, rule_id, 0.0f, 0, 0 };
    }

    static FirewallDecision alert_ai(float anomaly_score) {
        return { Action::ALERT, Reason::AI_ANOMALY, 0, anomaly_score, 0, 0 };
    }

    static FirewallDecision drop_rate_limit(uint32_t current, uint32_t limit) {
        return { Action::DROP, Reason::RATE_LIMIT, 0, 0.0f, current, limit };
    }

    // ─── 辅助方法 ───

    bool should_pass() const { 
        return action == Action::PASS || action == Action::ALERT || action == Action::LOG; 
    }

    bool should_drop() const { 
        return action == Action::DROP; 
    }

    bool is_alert() const {
        return action == Action::ALERT;
    }
};

static_assert(sizeof(FirewallDecision) == 16, "FirewallDecision should be 16 bytes");

} // namespace neustack

#endif // NEUSTACK_FIREWALL_FIREWALL_DECISION_HPP
