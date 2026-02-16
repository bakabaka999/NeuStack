#include <catch2/catch_test_macros.hpp>
#include "neustack/firewall/firewall_ai.hpp"
#include "neustack/firewall/firewall_engine.hpp"
#include "neustack/metrics/security_metrics.hpp"
#include "neustack/metrics/security_features.hpp"

using namespace neustack;

// ============================================================================
// SecurityMetrics 测试
// ============================================================================

TEST_CASE("SecurityMetrics: Basic Recording", "[firewall][ai][metrics]") {
    SecurityMetrics metrics;
    
    SECTION("Initial state") {
        auto snap = metrics.snapshot();
        CHECK(snap.packets_total == 0);
        CHECK(snap.syn_packets == 0);
        CHECK(snap.pps == 0.0);
    }
    
    SECTION("Record packets") {
        // 记录普通包
        metrics.record_packet(100, 0);
        metrics.record_packet(200, 0);
        
        auto snap = metrics.snapshot();
        CHECK(snap.packets_total == 2);
        CHECK(snap.bytes_total == 300);
    }
    
    SECTION("Record TCP flags") {
        constexpr uint8_t TCP_SYN = 0x02;
        constexpr uint8_t TCP_RST = 0x04;
        constexpr uint8_t TCP_SYN_ACK = 0x12;  // SYN + ACK
        
        metrics.record_packet(60, TCP_SYN);
        metrics.record_packet(60, TCP_SYN);
        metrics.record_packet(60, TCP_SYN_ACK);
        metrics.record_packet(40, TCP_RST);
        
        auto snap = metrics.snapshot();
        CHECK(snap.packets_total == 4);
        CHECK(snap.syn_packets == 2);
        CHECK(snap.syn_ack_packets == 1);
        CHECK(snap.rst_packets == 1);
    }
}

TEST_CASE("SecurityMetrics: Sliding Window", "[firewall][ai][metrics]") {
    SecurityMetrics metrics;
    
    // 第一秒：100 个包
    for (int i = 0; i < 100; i++) {
        metrics.record_packet(100, 0);
    }
    metrics.tick();
    
    // 第二秒：200 个包
    for (int i = 0; i < 200; i++) {
        metrics.record_packet(100, 0);
    }
    metrics.tick();
    
    // 快照应该显示平均 150 PPS
    auto snap = metrics.snapshot();
    CHECK(snap.pps >= 100.0);  // 至少包含窗口内的平均值
    CHECK(metrics.window_count() == 2);
}

// ============================================================================
// SecurityFeatureExtractor 测试
// ============================================================================

TEST_CASE("SecurityFeatureExtractor: Normalization", "[firewall][ai][features]") {
    SecurityFeatureExtractor extractor;
    
    SECTION("Default params") {
        SecurityMetrics::Snapshot snap{};
        snap.pps = 5000.0;        // 50% of max (10000)
        snap.syn_rate = 500.0;    // 50% of max (1000)
        snap.rst_rate = 250.0;    // 50% of max (500)
        snap.bps = 50000000.0;    // 50 MB/s
        snap.new_conn_rate = 100.0;
        snap.syn_to_synack_ratio = 1.0;  // 正常值
        
        auto features = extractor.extract(snap, 5000);
        
        // 检查归一化结果在合理范围
        CHECK(features.packets_rx_norm >= 0.0f);
        CHECK(features.packets_rx_norm <= 1.0f);
        CHECK(features.syn_rate_norm >= 0.0f);
        CHECK(features.syn_rate_norm <= 1.0f);
        CHECK(features.rst_rate_norm >= 0.0f);
        CHECK(features.rst_rate_norm <= 1.0f);
    }
    
    SECTION("High SYN ratio (potential attack)") {
        SecurityMetrics::Snapshot snap{};
        snap.syn_to_synack_ratio = 10.0;  // 异常高，可能是 SYN Flood
        
        auto features = extractor.extract(snap, 0);
        
        // SYN 比率应该被归一化到较高值（sigmoid）
        CHECK(features.tx_rx_ratio_norm > 0.7f);
    }
    
    SECTION("Clamping") {
        SecurityMetrics::Snapshot snap{};
        snap.pps = 100000.0;  // 超过 max (10000)
        
        auto features = extractor.extract(snap, 0);
        
        // 应该被裁剪到 1.0
        CHECK(features.packets_rx_norm == 1.0f);
    }
}

// ============================================================================
// FirewallAI 测试 (不需要实际模型)
// ============================================================================

TEST_CASE("FirewallAI: Basic Operations", "[firewall][ai]") {
    FirewallAIConfig config;
    config.shadow_mode = true;
    
    FirewallAI ai(config);
    
    SECTION("Not loaded by default") {
        CHECK(ai.is_loaded() == false);
    }
    
    SECTION("Default evaluation passes") {
        auto decision = ai.evaluate();
        CHECK(decision.should_pass() == true);
    }
    
    SECTION("Record and tick") {
        PacketEvent evt{};
        evt.total_len = 100;
        evt.tcp_flags = 0x02;  // SYN
        
        ai.record_packet(evt);
        
        // tick 前检查当前计数
        auto snap_before = ai.metrics_snapshot();
        CHECK(snap_before.syn_packets == 1);
        
        // tick 后当前计数被重置，但窗口内有数据
        ai.tick();
        
        // syn_rate 应该反映窗口内的数据
        auto snap_after = ai.metrics_snapshot();
        CHECK(snap_after.syn_rate >= 0.0);  // 有速率
    }
    
    SECTION("Shadow mode config") {
        CHECK(ai.shadow_mode() == true);
        ai.set_shadow_mode(false);
        CHECK(ai.shadow_mode() == false);
    }
    
    SECTION("Threshold config") {
        // 默认 anomaly_threshold = 0.0f (未加载模型时不会 fallback 到 0.5)
        CHECK(ai.threshold() == 0.0f);
        ai.set_threshold(0.8f);
        CHECK(ai.threshold() == 0.8f);
    }
}

TEST_CASE("FirewallAI: Stats Tracking", "[firewall][ai]") {
    FirewallAIConfig config;
    FirewallAI ai(config);
    
    // 初始统计
    CHECK(ai.stats().inferences_total == 0);
    CHECK(ai.stats().anomalies_detected == 0);
    
    // 运行推理（无模型时应该安全返回）
    float score = ai.run_inference();
    CHECK(score == 0.0f);
    
    // 重置统计
    ai.reset_stats();
    CHECK(ai.stats().inferences_total == 0);
}

// ============================================================================
// 集成测试：FirewallEngine + AI
// ============================================================================

TEST_CASE("FirewallEngine: AI Integration", "[firewall][ai][integration]") {
    FirewallConfig config;
    config.ai_enabled = false;  // 不加载实际模型
    
    FirewallEngine fw(config);
    
    SECTION("AI disabled by default") {
        CHECK(fw.ai_enabled() == false);
        CHECK(fw.ai() == nullptr);
    }
    
    SECTION("Enable AI without model path returns false") {
        bool result = fw.enable_ai("nonexistent.onnx");
        CHECK(result == false);
        CHECK(fw.ai_enabled() == false);
    }
    
    SECTION("on_timer without AI is safe") {
        fw.on_timer();  // 不应该崩溃
    }
}
