#include "neustack/firewall/firewall_engine.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/common/log.hpp"

#include <chrono>
#include <arpa/inet.h>

namespace neustack {

// ============================================================================
// 构造函数
// ============================================================================

FirewallEngine::FirewallEngine(const FirewallConfig& config)
    : _config(config)
{
    LOG_INFO(FW, "Firewall engine initialized (shadow_mode=%s)", 
             _config.shadow_mode ? "ON" : "OFF");
    
    // 如果配置了 AI，初始化 AI 模块
    if (_config.ai_enabled && !_config.ai_model_path.empty()) {
        enable_ai(_config.ai_model_path, _config.ai_threshold);
    }
}

// ============================================================================
// 核心 API
// ============================================================================

bool FirewallEngine::inspect(const uint8_t* data, size_t len) {
    // 防火墙关闭时直接放行
    if (!_config.enabled) {
        return true;
    }

    // 从池中获取 PacketEvent
    auto evt_ptr = _event_pool.acquire_ptr();
    if (!evt_ptr) {
        _stats.pool_acquire_failed++;
        LOG_WARN(FW, "PacketEvent pool exhausted, passing packet");
        return true;  // 池耗尽时放行，避免 DoS
    }

    // 解析数据包
    if (!parse_packet(data, len, evt_ptr.get())) {
        // 解析失败，可能是畸形包
        _stats.packets_inspected++;
        _stats.packets_dropped++;
        LOG_DEBUG(FW, "Malformed packet dropped");
        return false;
    }

    // 从池中获取 Decision
    auto dec_ptr = _decision_pool.acquire_ptr();
    if (!dec_ptr) {
        _stats.pool_acquire_failed++;
        LOG_WARN(FW, "FirewallDecision pool exhausted, passing packet");
        return true;
    }

    // 执行评估
    *dec_ptr = evaluate(*evt_ptr);

    // 记录决策
    record_decision(*evt_ptr, *dec_ptr);

    // 返回结果
    return dec_ptr->should_pass();
}

bool FirewallEngine::inspect(const IPv4Packet& pkt) {
    if (!_config.enabled) {
        return true;
    }

    auto evt_ptr = _event_pool.acquire_ptr();
    if (!evt_ptr) {
        _stats.pool_acquire_failed++;
        return true;
    }

    fill_event_from_ipv4(pkt, evt_ptr.get());

    auto dec_ptr = _decision_pool.acquire_ptr();
    if (!dec_ptr) {
        _stats.pool_acquire_failed++;
        return true;
    }

    *dec_ptr = evaluate(*evt_ptr);
    record_decision(*evt_ptr, *dec_ptr);

    return dec_ptr->should_pass();
}

// ============================================================================
// 内部方法：数据包解析
// ============================================================================

bool FirewallEngine::parse_packet(const uint8_t* data, size_t len, PacketEvent* evt) {
    // 最小长度检查：IPv4 头至少 20 字节
    if (len < 20) {
        return false;
    }

    // 检查版本号
    uint8_t version = (data[0] >> 4) & 0x0F;
    if (version != 4) {
        return false;  // 只处理 IPv4
    }

    // 解析 IPv4 头
    const auto* ip = reinterpret_cast<const IPv4Header*>(data);
    size_t ip_header_len = ip->header_length();

    if (ip_header_len < 20 || ip_header_len > len) {
        return false;
    }

    uint16_t total_len = ntohs(ip->total_length);
    if (total_len > len) {
        return false;
    }

    // 填充 PacketEvent
    evt->src_ip = ip->src_addr;
    evt->dst_ip = ip->dst_addr;
    evt->protocol = ip->protocol;
    evt->total_len = total_len;
    evt->timestamp_us = now_us();
    evt->raw_packet = data;

    // 默认值
    evt->src_port = 0;
    evt->dst_port = 0;
    evt->tcp_flags = 0;
    evt->payload_len = 0;
    evt->_reserved = 0;

    const uint8_t* transport_hdr = data + ip_header_len;
    size_t transport_len = total_len - ip_header_len;

    // 解析传输层
    if (ip->protocol == 6 && transport_len >= 20) {
        // TCP
        evt->src_port = ntohs(*reinterpret_cast<const uint16_t*>(transport_hdr));
        evt->dst_port = ntohs(*reinterpret_cast<const uint16_t*>(transport_hdr + 2));
        evt->tcp_flags = transport_hdr[13];  // TCP flags 在偏移 13

        uint8_t tcp_data_offset = (transport_hdr[12] >> 4) * 4;
        if (tcp_data_offset <= transport_len) {
            evt->payload_len = transport_len - tcp_data_offset;
        }
    } else if (ip->protocol == 17 && transport_len >= 8) {
        // UDP
        evt->src_port = ntohs(*reinterpret_cast<const uint16_t*>(transport_hdr));
        evt->dst_port = ntohs(*reinterpret_cast<const uint16_t*>(transport_hdr + 2));
        evt->payload_len = transport_len - 8;
    } else if (ip->protocol == 1) {
        // ICMP - 没有端口概念
        evt->payload_len = transport_len;
    }

    return true;
}

void FirewallEngine::fill_event_from_ipv4(const IPv4Packet& pkt, PacketEvent* evt) {
    evt->src_ip = htonl(pkt.src_addr);  // IPv4Packet 是主机字节序，转回网络字节序
    evt->dst_ip = htonl(pkt.dst_addr);
    evt->protocol = pkt.protocol;
    evt->total_len = pkt.total_length;
    evt->timestamp_us = now_us();
    evt->raw_packet = pkt.raw_data;

    evt->src_port = 0;
    evt->dst_port = 0;
    evt->tcp_flags = 0;
    evt->payload_len = pkt.payload_length;
    evt->_reserved = 0;

    // 如果是 TCP/UDP，需要从 payload 中解析端口
    if (pkt.payload && pkt.payload_length >= 8) {
        if (pkt.protocol == 6 || pkt.protocol == 17) {
            evt->src_port = ntohs(*reinterpret_cast<const uint16_t*>(pkt.payload));
            evt->dst_port = ntohs(*reinterpret_cast<const uint16_t*>(pkt.payload + 2));
        }
        if (pkt.protocol == 6 && pkt.payload_length >= 14) {
            evt->tcp_flags = pkt.payload[13];
        }
    }
}

// ============================================================================
// 内部方法：规则评估
// ============================================================================

FirewallDecision FirewallEngine::evaluate(const PacketEvent& evt) {
    // 1. 规则引擎检查
    auto decision = _rule_engine.evaluate(evt, now_us());
    
    // 如果规则引擎已经决定 DROP，直接返回
    if (decision.should_drop()) {
        return decision;
    }
    
    // 2. AI 检测（如果启用）
    // 无论模型是否加载，都持续采集指标，避免窗口空洞
    if (_ai) {
        _ai->record_packet(evt);
    }

    if (_ai && _ai->is_loaded()) {
        // 获取 AI 决策（使用缓存的推理结果）
        auto ai_decision = _ai->evaluate();
        
        // 如果 AI 检测到异常
        if (ai_decision.is_alert() || ai_decision.should_drop()) {
            return ai_decision;
        }
    }
    
    // 3. 默认使用规则引擎的决策
    return decision;
}

// ============================================================================
// AI API
// ============================================================================

bool FirewallEngine::enable_ai(const std::string& model_path, float threshold) {
    FirewallAIConfig ai_config;
    ai_config.model_path = model_path;
    ai_config.anomaly_threshold = threshold;
    ai_config.shadow_mode = _config.shadow_mode;
    
    _ai = std::make_unique<FirewallAI>(ai_config);
    
    if (_ai->is_loaded()) {
        LOG_INFO(FW, "AI enabled: model=%s threshold=%.3f", 
                 model_path.c_str(), threshold);
        return true;
    } else {
        LOG_WARN(FW, "Failed to enable AI: model=%s", model_path.c_str());
        _ai.reset();
        return false;
    }
}

void FirewallEngine::disable_ai() {
    if (_ai) {
        LOG_INFO(FW, "AI disabled");
        _ai.reset();
    }
}

void FirewallEngine::on_timer() {
    if (!_ai) return;
    
    // 每秒更新 AI 指标窗口
    _ai->tick();
    _tick_count++;
    
    // 按配置间隔执行 AI 推理
    // on_timer() 每秒调用，inference_interval_ms / 1000 = 每几个 tick 推理一次
    if (_ai->is_loaded()) {
        uint32_t ticks_per_inference = std::max(
            1u, _ai->inference_interval_ms() / 1000u);
        if (_tick_count % ticks_per_inference == 0) {
            _ai->run_inference();
        }
    }
}

// ============================================================================
// 内部方法：决策记录
// ============================================================================

void FirewallEngine::record_decision(const PacketEvent& evt, const FirewallDecision& decision) {
    _stats.packets_inspected++;

    switch (decision.action) {
        case FirewallDecision::Action::PASS:
        case FirewallDecision::Action::LOG:
            _stats.packets_passed++;
            break;
        case FirewallDecision::Action::DROP:
            _stats.packets_dropped++;
            break;
        case FirewallDecision::Action::ALERT:
            _stats.packets_alerted++;
            _stats.packets_passed++;  // Alert 也算放行
            break;
    }

    // 调用用户回调
    if (_on_decision) {
        _on_decision(evt, decision);
    }
}

// ============================================================================
// 内部方法：时间戳
// ============================================================================

uint64_t FirewallEngine::now_us() const {
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    );
    return static_cast<uint64_t>(us.count());
}

} // namespace neustack
