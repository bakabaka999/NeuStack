#ifndef NEUSTACK_FIREWALL_RULE_ENGINE_HPP
#define NEUSTACK_FIREWALL_RULE_ENGINE_HPP

#include "neustack/firewall/rule.hpp"
#include "neustack/firewall/rate_limiter.hpp"
#include "neustack/firewall/firewall_decision.hpp"
#include "neustack/firewall/packet_event.hpp"

#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cstdint>

namespace neustack {

/**
 * 规则引擎 - 管理和匹配防火墙规则
 *
 * 功能:
 * 1. 规则管理 (增删改查)
 * 2. 黑白名单快速匹配 (O(1) 哈希查找)
 * 3. 通用规则匹配 (按优先级顺序)
 * 4. 速率限制 (令牌桶)
 *
 * 匹配优先级:
 * 1. 白名单 (最高) - 直接放行
 * 2. 黑名单 - 直接拒绝
 * 3. 速率限制 - 超限拒绝
 * 4. 通用规则 - 按 priority 排序匹配
 * 5. 默认放行
 */
class RuleEngine {
public:
    static constexpr size_t MAX_RULES = 1024;

    struct Stats {
        uint64_t whitelist_hits = 0;
        uint64_t blacklist_hits = 0;
        uint64_t rate_limit_drops = 0;
        uint64_t rule_matches = 0;
        uint64_t default_passes = 0;
    };

    explicit RuleEngine() = default;

    // ─── 规则管理 ───

    /**
     * @brief 添加规则
     * @return true 如果添加成功
     */
    bool add_rule(const Rule& rule) {
        if (_rules.size() >= MAX_RULES) return false;
        
        _rules.push_back(rule);
        sort_rules();
        return true;
    }

    /**
     * @brief 按 ID 移除规则
     */
    bool remove_rule(uint16_t rule_id) {
        auto it = std::remove_if(_rules.begin(), _rules.end(),
            [rule_id](const Rule& r) { return r.id == rule_id; });
        
        if (it != _rules.end()) {
            _rules.erase(it, _rules.end());
            return true;
        }
        return false;
    }

    /**
     * @brief 启用/禁用规则
     */
    bool set_rule_enabled(uint16_t rule_id, bool enabled) {
        for (auto& rule : _rules) {
            if (rule.id == rule_id) {
                rule.enabled = enabled;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 清空所有规则
     */
    void clear_rules() {
        _rules.clear();
    }

    size_t rule_count() const { return _rules.size(); }

    // ─── 黑白名单快速路径 ───

    void add_whitelist_ip(uint32_t ip) { _whitelist_ips.insert(ip); }
    void remove_whitelist_ip(uint32_t ip) { _whitelist_ips.erase(ip); }
    void clear_whitelist() { _whitelist_ips.clear(); }
    size_t whitelist_size() const { return _whitelist_ips.size(); }

    void add_blacklist_ip(uint32_t ip) { _blacklist_ips.insert(ip); }
    void remove_blacklist_ip(uint32_t ip) { _blacklist_ips.erase(ip); }
    void clear_blacklist() { _blacklist_ips.clear(); }
    size_t blacklist_size() const { return _blacklist_ips.size(); }

    // ─── 速率限制 ───

    RateLimiter& rate_limiter() { return _rate_limiter; }
    const RateLimiter& rate_limiter() const { return _rate_limiter; }

    // ─── 核心匹配 ───

    /**
     * @brief 评估数据包，返回决策
     *
     * @param evt 解析后的包事件
     * @param now_us 当前时间 (微秒)
     * @return FirewallDecision 决策结果
     */
    FirewallDecision evaluate(const PacketEvent& evt, uint64_t now_us) {
        // 1. 白名单快速路径 (O(1))
        if (_whitelist_ips.count(evt.src_ip)) {
            _stats.whitelist_hits++;
            return FirewallDecision{
                .action = FirewallDecision::Action::PASS,
                .reason = FirewallDecision::Reason::RULE_WHITELIST,
                .rule_id = 0,
                .score = 0.0f,
                .rate_current = 0,
                .rate_limit = 0
            };
        }

        // 2. 黑名单快速路径 (O(1))
        if (_blacklist_ips.count(evt.src_ip)) {
            _stats.blacklist_hits++;
            return FirewallDecision{
                .action = FirewallDecision::Action::DROP,
                .reason = FirewallDecision::Reason::RULE_BLACKLIST,
                .rule_id = 0,
                .score = 0.0f,
                .rate_current = 0,
                .rate_limit = 0
            };
        }

        // 3. 速率限制检查
        if (_rate_limiter.enabled()) {
            auto result = _rate_limiter.check(evt.src_ip, now_us);
            if (!result.allowed) {
                _stats.rate_limit_drops++;
                return FirewallDecision{
                    .action = FirewallDecision::Action::DROP,
                    .reason = FirewallDecision::Reason::RATE_LIMIT,
                    .rule_id = 0,
                    .score = 0.0f,
                    .rate_current = result.current_pps,
                    .rate_limit = result.limit_pps
                };
            }
        }

        // 4. 通用规则匹配 (按优先级顺序)
        for (const auto& rule : _rules) {
            if (rule.matches(evt.src_ip, evt.dst_ip, evt.src_port, evt.dst_port, evt.protocol)) {
                _stats.rule_matches++;
                return FirewallDecision{
                    .action = rule.action,
                    .reason = rule.reason,
                    .rule_id = rule.id,
                    .score = 0.0f,
                    .rate_current = 0,
                    .rate_limit = 0
                };
            }
        }

        // 5. 默认放行
        _stats.default_passes++;
        return FirewallDecision::pass();
    }

    // ─── 统计 ───

    const Stats& stats() const { return _stats; }
    void reset_stats() { _stats = {}; }

private:
    void sort_rules() {
        std::sort(_rules.begin(), _rules.end(),
            [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
    }

    std::vector<Rule> _rules;
    std::unordered_set<uint32_t> _whitelist_ips;
    std::unordered_set<uint32_t> _blacklist_ips;
    RateLimiter _rate_limiter;
    Stats _stats;
};

} // namespace neustack

#endif // NEUSTACK_FIREWALL_RULE_ENGINE_HPP
