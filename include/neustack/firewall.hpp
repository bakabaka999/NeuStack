#ifndef NEUSTACK_FIREWALL_HPP
#define NEUSTACK_FIREWALL_HPP

/**
 * NeuStack Firewall Module
 *
 * 统一头文件，包含防火墙模块的所有公开接口
 */

// 核心组件
#include "neustack/firewall/packet_event.hpp"
#include "neustack/firewall/firewall_decision.hpp"
#include "neustack/firewall/rule.hpp"
#include "neustack/firewall/rate_limiter.hpp"
#include "neustack/firewall/rule_engine.hpp"
#include "neustack/firewall/firewall_ai.hpp"
#include "neustack/firewall/firewall_engine.hpp"

// 指标和特征（位于 metrics/ 目录）
#include "neustack/metrics/security_metrics.hpp"
#include "neustack/metrics/security_features.hpp"

#endif // NEUSTACK_FIREWALL_HPP
