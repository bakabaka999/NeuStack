/**
 * 集成测试：防火墙 AI Shadow Mode
 *
 * 验证 AI 异常检测与 Shadow Mode 的工作流程：
 * 1. AI 模块加载（无模型时安全降级）
 * 2. 指标收集和特征提取
 * 3. Shadow Mode 告警逻辑
 */

#include "neustack/firewall.hpp"
#include "neustack/common/log.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include <arpa/inet.h>

using namespace neustack;

// ============================================================================
// 辅助函数：构造数据包
// ============================================================================

#pragma pack(push, 1)
struct RawIPv4Header {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_addr;
    uint32_t dst_addr;
};

struct RawTCPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
};
#pragma pack(pop)

static constexpr uint32_t IP(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

static std::vector<uint8_t> make_tcp_syn(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port) {
    std::vector<uint8_t> packet(40, 0);
    
    auto* ip = reinterpret_cast<RawIPv4Header*>(packet.data());
    ip->version_ihl = 0x45;
    ip->total_length = htons(40);
    ip->ttl = 64;
    ip->protocol = 6;
    ip->src_addr = htonl(src_ip);
    ip->dst_addr = htonl(dst_ip);
    
    auto* tcp = reinterpret_cast<RawTCPHeader*>(packet.data() + 20);
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->data_offset = 0x50;
    tcp->flags = 0x02;  // SYN
    tcp->window = htons(65535);
    
    return packet;
}

static std::vector<uint8_t> make_tcp_rst(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port) {
    std::vector<uint8_t> packet(40, 0);
    
    auto* ip = reinterpret_cast<RawIPv4Header*>(packet.data());
    ip->version_ihl = 0x45;
    ip->total_length = htons(40);
    ip->ttl = 64;
    ip->protocol = 6;
    ip->src_addr = htonl(src_ip);
    ip->dst_addr = htonl(dst_ip);
    
    auto* tcp = reinterpret_cast<RawTCPHeader*>(packet.data() + 20);
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->data_offset = 0x50;
    tcp->flags = 0x04;  // RST
    tcp->window = htons(65535);
    
    return packet;
}

// ============================================================================
// 测试框架
// ============================================================================

static int g_failures = 0;
static int g_passes = 0;

static void check(bool cond, const char* msg) {
    if (cond) {
        std::printf("  PASS: %s\n", msg);
        g_passes++;
    } else {
        std::printf("  FAIL: %s\n", msg);
        g_failures++;
    }
}

// ============================================================================
// 测试用例
// ============================================================================

static void test_ai_graceful_degradation() {
    std::printf("\n[Test] AI Graceful Degradation (no model)\n");
    
    FirewallConfig config;
    config.ai_enabled = false;
    
    FirewallEngine fw(config);
    
    // 无模型时，AI 应该未启用
    check(fw.ai_enabled() == false, "AI not enabled without model");
    check(fw.ai() == nullptr, "AI pointer is null");
    
    // 尝试启用不存在的模型
    bool result = fw.enable_ai("nonexistent_model.onnx", 0.5f);
    check(result == false, "enable_ai returns false for missing model");
    check(fw.ai_enabled() == false, "AI still disabled after failed load");
    
    // 防火墙应该正常工作（降级到纯规则模式）
    auto pkt = make_tcp_syn(IP(192,168,1,100), IP(10,0,0,1), 54321, 80);
    check(fw.inspect(pkt.data(), pkt.size()) == true, "Packets pass without AI");
    
    // on_timer 不应该崩溃
    fw.on_timer();
    check(true, "on_timer() safe without AI");
}

static void test_security_metrics_collection() {
    std::printf("\n[Test] Security Metrics Collection\n");
    
    FirewallConfig config;
    FirewallEngine fw(config);
    
    // 发送混合流量
    for (int i = 0; i < 50; i++) {
        auto syn = make_tcp_syn(IP(10,0,0,i % 10), IP(192,168,1,1), 40000 + i, 80);
        fw.inspect(syn.data(), syn.size());
    }
    
    for (int i = 0; i < 20; i++) {
        auto rst = make_tcp_rst(IP(10,0,0,i % 5), IP(192,168,1,1), 40000 + i, 80);
        fw.inspect(rst.data(), rst.size());
    }
    
    // 检查统计
    check(fw.stats().packets_inspected == 70, "70 packets inspected");
    check(fw.stats().packets_passed == 70, "70 packets passed (no rules)");
}

static void test_ai_with_model() {
    std::printf("\n[Test] AI with Model (if available)\n");
    
    FirewallConfig config;
    config.shadow_mode = true;
    
    FirewallEngine fw(config);
    
    // 尝试加载真实模型
    bool loaded = fw.enable_ai("models/anomaly_detector.onnx", 0.3f);
    
    if (!loaded) {
        std::printf("  SKIP: Model not found or AI not compiled\n");
        return;
    }
    
    check(fw.ai_enabled() == true, "AI enabled with model");
    check(fw.ai() != nullptr, "AI pointer valid");
    check(fw.ai()->shadow_mode() == true, "Shadow mode inherited");
    
    // 发送正常流量
    for (int i = 0; i < 100; i++) {
        auto pkt = make_tcp_syn(IP(192,168,1,i % 50), IP(10,0,0,1), 40000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }
    
    // 触发 AI 推理
    fw.on_timer();
    
    // 检查 AI 统计
    auto& ai_stats = fw.ai()->stats();
    check(ai_stats.inferences_total >= 1, "At least one inference ran");
    
    std::printf("  INFO: AI score = %.4f, anomalies = %llu\n",
                ai_stats.last_anomaly_score, 
                static_cast<unsigned long long>(ai_stats.anomalies_detected));
}

static void test_shadow_mode_alert() {
    std::printf("\n[Test] Shadow Mode Alert (simulated)\n");
    
    // 这个测试验证 Shadow Mode 的逻辑，不依赖真实模型
    FirewallAIConfig ai_config;
    ai_config.shadow_mode = true;
    ai_config.anomaly_threshold = 0.5f;
    
    FirewallAI ai(ai_config);
    
    // Shadow Mode ON: evaluate 应该返回 PASS（无模型时）
    auto decision = ai.evaluate();
    check(decision.should_pass() == true, "No model -> PASS");
    
    // 验证 shadow_mode 开关
    check(ai.shadow_mode() == true, "Shadow mode is ON");
    ai.set_shadow_mode(false);
    check(ai.shadow_mode() == false, "Shadow mode toggled OFF");
}

static void test_metrics_window() {
    std::printf("\n[Test] Metrics Sliding Window\n");
    
    SecurityMetrics metrics;
    
    // 第一秒：记录流量
    for (int i = 0; i < 100; i++) {
        metrics.record_packet(100, 0x02);  // SYN
    }
    
    auto snap1 = metrics.snapshot();
    check(snap1.syn_packets == 100, "100 SYN packets in current window");
    
    // tick: 移动窗口
    metrics.tick();
    
    // 第二秒：当前窗口清空，但历史窗口有数据
    auto snap2 = metrics.snapshot();
    check(snap2.syn_packets == 0, "Current window reset after tick");
    check(snap2.syn_rate > 0, "SYN rate reflects historical window");
    
    std::printf("  INFO: syn_rate = %.2f/s\n", snap2.syn_rate);
}

static void test_feature_extraction() {
    std::printf("\n[Test] Feature Extraction\n");
    
    SecurityFeatureExtractor extractor;
    
    // 正常流量特征
    SecurityMetrics::Snapshot normal{};
    normal.pps = 1000.0;
    normal.syn_rate = 50.0;
    normal.rst_rate = 10.0;
    normal.bps = 1000000.0;
    normal.syn_to_synack_ratio = 1.2;
    
    auto features = extractor.extract(normal, 100);
    
    check(features.packets_rx_norm >= 0.0f && features.packets_rx_norm <= 1.0f,
          "packets_rx_norm in [0,1]");
    check(features.syn_rate_norm >= 0.0f && features.syn_rate_norm <= 1.0f,
          "syn_rate_norm in [0,1]");
    
    // 异常流量特征（高 SYN 比率）
    SecurityMetrics::Snapshot attack{};
    attack.pps = 5000.0;
    attack.syn_rate = 4000.0;  // 很高的 SYN 速率
    attack.rst_rate = 100.0;
    attack.syn_to_synack_ratio = 50.0;  // 异常高
    
    auto attack_features = extractor.extract(attack, 1000);
    
    check(attack_features.syn_rate_norm > features.syn_rate_norm,
          "Attack has higher syn_rate_norm");
    check(attack_features.tx_rx_ratio_norm > 0.9f,
          "High SYN ratio normalized > 0.9");
    
    std::printf("  INFO: normal syn_rate_norm=%.3f, attack syn_rate_norm=%.3f\n",
                features.syn_rate_norm, attack_features.syn_rate_norm);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    auto& logger = Logger::instance();
    logger.set_level(LogLevel::WARN);

    std::printf("=== Firewall AI Shadow Mode Integration Test ===\n");

    test_ai_graceful_degradation();
    test_security_metrics_collection();
    test_shadow_mode_alert();
    test_metrics_window();
    test_feature_extraction();
    test_ai_with_model();  // 最后运行，可能跳过

    std::printf("\n=================================================\n");
    std::printf("Passed: %d, Failed: %d\n", g_passes, g_failures);
    std::printf("%s\n", g_failures == 0 ? "=== ALL PASSED ===" : "=== SOME TESTS FAILED ===");

    return g_failures;
}
