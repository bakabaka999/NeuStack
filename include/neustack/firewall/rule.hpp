#ifndef NEUSTACK_FIREWALL_RULE_HPP
#define NEUSTACK_FIREWALL_RULE_HPP

#include "neustack/firewall/firewall_decision.hpp"
#include <cstdint>

namespace neustack {

/**
 * 防火墙规则 - 单条匹配规则
 *
 * 设计原则:
 * - 固定大小，便于数组存储
 * - 支持通配符 (0 = any)
 * - 优先级排序 (priority 越小越优先)
 */
struct Rule {
    // ─── 匹配条件 ───
    uint32_t src_ip;          // 源 IP (0 = any)
    uint32_t src_mask;        // 源 IP 掩码 (0xFFFFFFFF = exact match)
    uint32_t dst_ip;          // 目的 IP (0 = any)
    uint32_t dst_mask;        // 目的 IP 掩码
    uint16_t src_port;        // 源端口 (0 = any)
    uint16_t dst_port;        // 目的端口 (0 = any)
    uint8_t  protocol;        // 协议号 (0 = any, 6 = TCP, 17 = UDP)
    
    // ─── 规则属性 ───
    uint16_t id;                          // 规则 ID
    uint16_t priority;                    // 优先级 (越小越优先)
    FirewallDecision::Action action;      // 动作
    FirewallDecision::Reason reason;      // 原因分类
    bool enabled;                         // 是否启用

    // ─── 匹配方法 ───
    
    /**
     * @brief 检查规则是否匹配给定的包
     */
    bool matches(uint32_t pkt_src_ip, uint32_t pkt_dst_ip,
                 uint16_t pkt_src_port, uint16_t pkt_dst_port,
                 uint8_t pkt_protocol) const 
    {
        if (!enabled) return false;
        
        // IP 匹配 (带掩码)
        if (src_ip != 0 && ((pkt_src_ip & src_mask) != (src_ip & src_mask))) 
            return false;
        if (dst_ip != 0 && ((pkt_dst_ip & dst_mask) != (dst_ip & dst_mask))) 
            return false;
        
        // 端口匹配
        if (src_port != 0 && pkt_src_port != src_port) return false;
        if (dst_port != 0 && pkt_dst_port != dst_port) return false;
        
        // 协议匹配
        if (protocol != 0 && pkt_protocol != protocol) return false;
        
        return true;
    }

    // ─── 工厂方法 ───

    /**
     * @brief 创建 IP 黑名单规则
     */
    static Rule blacklist_ip(uint16_t id, uint32_t ip, uint16_t priority = 100) {
        return Rule{
            .src_ip = ip,
            .src_mask = 0xFFFFFFFF,
            .dst_ip = 0,
            .dst_mask = 0,
            .src_port = 0,
            .dst_port = 0,
            .protocol = 0,
            .id = id,
            .priority = priority,
            .action = FirewallDecision::Action::DROP,
            .reason = FirewallDecision::Reason::RULE_BLACKLIST,
            .enabled = true
        };
    }

    /**
     * @brief 创建 IP 白名单规则
     */
    static Rule whitelist_ip(uint16_t id, uint32_t ip, uint16_t priority = 50) {
        return Rule{
            .src_ip = ip,
            .src_mask = 0xFFFFFFFF,
            .dst_ip = 0,
            .dst_mask = 0,
            .src_port = 0,
            .dst_port = 0,
            .protocol = 0,
            .id = id,
            .priority = priority,
            .action = FirewallDecision::Action::PASS,
            .reason = FirewallDecision::Reason::RULE_WHITELIST,
            .enabled = true
        };
    }

    /**
     * @brief 创建端口封禁规则
     */
    static Rule block_port(uint16_t id, uint16_t port, uint8_t proto = 0, uint16_t priority = 100) {
        return Rule{
            .src_ip = 0,
            .src_mask = 0,
            .dst_ip = 0,
            .dst_mask = 0,
            .src_port = 0,
            .dst_port = port,
            .protocol = proto,
            .id = id,
            .priority = priority,
            .action = FirewallDecision::Action::DROP,
            .reason = FirewallDecision::Reason::RULE_PORT,
            .enabled = true
        };
    }
};

} // namespace neustack

#endif // NEUSTACK_FIREWALL_RULE_HPP
