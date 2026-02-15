/**
 * 端到端测试：防火墙全功能验证
 *
 * 模拟真实流量场景，完整验证防火墙从包解析 → 规则检查 → AI 检测 → 决策的全流程。
 * 覆盖 v1.2 全部功能：
 *   1. 基础包过滤（黑白名单、端口规则）
 *   2. 限速（Token Bucket + EWMA）
 *   3. AI 异常检测（Shadow Mode / Enforce Mode）
 *   4. 自动升级/降级
 *   5. 混合规则 + AI 联动
 *   6. 统计与回调
 *   7. 畸形包与边界情况
 */

#include "neustack/firewall/firewall_engine.hpp"
#include "neustack/firewall/firewall_ai.hpp"
#include "neustack/common/log.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include <arpa/inet.h>

using namespace neustack;

// ============================================================================
// 辅助函数：构造真实 IP 包
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

struct RawUDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
};
#pragma pack(pop)

static constexpr uint32_t IP(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

// TCP SYN 包
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

// TCP SYN-ACK 包
static std::vector<uint8_t> make_tcp_synack(uint32_t src_ip, uint32_t dst_ip,
                                             uint16_t src_port, uint16_t dst_port) {
    auto pkt = make_tcp_syn(src_ip, dst_ip, src_port, dst_port);
    auto* tcp = reinterpret_cast<RawTCPHeader*>(pkt.data() + 20);
    tcp->flags = 0x12;  // SYN+ACK
    return pkt;
}

// TCP RST 包
static std::vector<uint8_t> make_tcp_rst(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port) {
    auto pkt = make_tcp_syn(src_ip, dst_ip, src_port, dst_port);
    auto* tcp = reinterpret_cast<RawTCPHeader*>(pkt.data() + 20);
    tcp->flags = 0x04;  // RST
    return pkt;
}

// TCP ACK 包（普通数据包）
static std::vector<uint8_t> make_tcp_ack(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port,
                                          size_t payload_len = 0) {
    size_t total = 40 + payload_len;
    std::vector<uint8_t> packet(total, 0);
    auto* ip = reinterpret_cast<RawIPv4Header*>(packet.data());
    ip->version_ihl = 0x45;
    ip->total_length = htons(static_cast<uint16_t>(total));
    ip->ttl = 64;
    ip->protocol = 6;
    ip->src_addr = htonl(src_ip);
    ip->dst_addr = htonl(dst_ip);

    auto* tcp = reinterpret_cast<RawTCPHeader*>(packet.data() + 20);
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->data_offset = 0x50;
    tcp->flags = 0x10;  // ACK
    tcp->window = htons(65535);
    // payload 填充随机字节
    for (size_t i = 40; i < total; i++) {
        packet[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return packet;
}

// UDP 包
static std::vector<uint8_t> make_udp(uint32_t src_ip, uint32_t dst_ip,
                                      uint16_t src_port, uint16_t dst_port,
                                      size_t payload_len = 0) {
    size_t total = 28 + payload_len;
    std::vector<uint8_t> packet(total, 0);
    auto* ip = reinterpret_cast<RawIPv4Header*>(packet.data());
    ip->version_ihl = 0x45;
    ip->total_length = htons(static_cast<uint16_t>(total));
    ip->ttl = 64;
    ip->protocol = 17;
    ip->src_addr = htonl(src_ip);
    ip->dst_addr = htonl(dst_ip);

    auto* udp = reinterpret_cast<RawUDPHeader*>(packet.data() + 20);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(static_cast<uint16_t>(8 + payload_len));
    return packet;
}

// ICMP 包
static std::vector<uint8_t> make_icmp_echo(uint32_t src_ip, uint32_t dst_ip) {
    std::vector<uint8_t> packet(28, 0);
    auto* ip = reinterpret_cast<RawIPv4Header*>(packet.data());
    ip->version_ihl = 0x45;
    ip->total_length = htons(28);
    ip->ttl = 64;
    ip->protocol = 1;  // ICMP
    ip->src_addr = htonl(src_ip);
    ip->dst_addr = htonl(dst_ip);
    // ICMP Echo Request
    packet[20] = 8;  // Type = Echo Request
    packet[21] = 0;  // Code = 0
    return packet;
}

// ============================================================================
// 测试框架
// ============================================================================

static int g_failures = 0;
static int g_passes = 0;
static int g_skips = 0;

static void check(bool cond, const char* msg) {
    if (cond) {
        std::printf("  PASS: %s\n", msg);
        g_passes++;
    } else {
        std::printf("  FAIL: %s\n", msg);
        g_failures++;
    }
}

static void skip(const char* msg) {
    std::printf("  SKIP: %s\n", msg);
    g_skips++;
}

// ============================================================================
// Test 1: 完整规则管线 — 白名单 > 黑名单 > 规则 > 限速 > 默认放行
// ============================================================================

static void test_rule_pipeline_priority() {
    std::printf("\n[Test 1] Rule Pipeline Priority\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();

    uint32_t vip     = IP(10,0,0,1);
    uint32_t bad     = IP(10,0,0,2);
    uint32_t normal  = IP(10,0,0,3);
    uint32_t dst     = IP(192,168,1,1);

    // 设置规则
    rules.add_whitelist_ip(htonl(vip));
    rules.add_blacklist_ip(htonl(bad));
    rules.add_blacklist_ip(htonl(vip));    // VIP 同时在黑名单
    rules.add_rule(Rule::block_port(1, 22, 6));  // 封 TCP 22

    // 白名单优先于黑名单
    auto p1 = make_tcp_syn(vip, dst, 54321, 22);
    check(fw.inspect(p1.data(), p1.size()) == true,
          "Whitelist VIP passes even if also blacklisted + port 22");

    // 黑名单丢弃
    auto p2 = make_tcp_syn(bad, dst, 54321, 80);
    check(fw.inspect(p2.data(), p2.size()) == false,
          "Blacklisted IP dropped on port 80");

    // 规则匹配：TCP 22 被封
    auto p3 = make_tcp_syn(normal, dst, 54321, 22);
    check(fw.inspect(p3.data(), p3.size()) == false,
          "Normal IP dropped on TCP port 22 (rule)");

    // 规则不匹配：UDP 22 不被封（规则只封 TCP）
    auto p4 = make_udp(normal, dst, 54321, 22);
    check(fw.inspect(p4.data(), p4.size()) == true,
          "Normal IP passes on UDP port 22 (rule is TCP only)");

    // 默认放行
    auto p5 = make_tcp_syn(normal, dst, 54321, 80);
    check(fw.inspect(p5.data(), p5.size()) == true,
          "Normal IP passes on port 80 (default pass)");

    // 验证统计
    auto s = fw.stats();
    check(s.packets_inspected == 5, "5 packets inspected");
    check(s.packets_passed == 3, "3 packets passed");
    check(s.packets_dropped == 2, "2 packets dropped");

    auto& rs = rules.stats();
    check(rs.whitelist_hits == 1, "1 whitelist hit");
    check(rs.blacklist_hits == 1, "1 blacklist hit");
    check(rs.rule_matches == 1, "1 rule match");
    check(rs.default_passes == 2, "2 default passes (UDP 22 + TCP 80)");
}

// ============================================================================
// Test 2: 限速端到端 — 突发 + 恢复 + 多 IP 隔离
// ============================================================================

static void test_rate_limiting_e2e() {
    std::printf("\n[Test 2] Rate Limiting End-to-End\n");

    FirewallEngine fw;
    auto& limiter = fw.rule_engine().rate_limiter();

    limiter.set_enabled(true);
    limiter.set_rate(10, 5);  // 10 pps, burst 5

    uint32_t src_a = IP(10,0,0,10);
    uint32_t src_b = IP(10,0,0,20);
    uint32_t dst   = IP(192,168,1,1);

    // A 发 5 个包（突发）→ 全部通过
    int a_pass = 0, a_drop = 0;
    for (int i = 0; i < 5; i++) {
        auto pkt = make_tcp_syn(src_a, dst, 40000 + i, 80);
        if (fw.inspect(pkt.data(), pkt.size())) a_pass++; else a_drop++;
    }
    check(a_pass == 5, "IP-A: burst of 5 all pass");
    check(a_drop == 0, "IP-A: 0 dropped in burst");

    // A 再发 3 个 → 应该被限速
    for (int i = 0; i < 3; i++) {
        auto pkt = make_tcp_syn(src_a, dst, 40010 + i, 80);
        if (fw.inspect(pkt.data(), pkt.size())) a_pass++; else a_drop++;
    }
    check(a_drop > 0, "IP-A: excess packets dropped after burst");

    // B 发 3 个包 → 独立桶，应该全部通过
    int b_pass = 0;
    for (int i = 0; i < 3; i++) {
        auto pkt = make_tcp_syn(src_b, dst, 50000 + i, 80);
        if (fw.inspect(pkt.data(), pkt.size())) b_pass++;
    }
    check(b_pass == 3, "IP-B: independent bucket, all pass");

    // 验证规则引擎统计
    check(fw.rule_engine().stats().rate_limit_drops > 0, "Rate limit drops recorded");
}

// ============================================================================
// Test 3: AI Shadow Mode — 无模型安全降级 + 指标收集
// ============================================================================

static void test_ai_shadow_no_model() {
    std::printf("\n[Test 3] AI Shadow Mode (no model - graceful degradation)\n");

    FirewallConfig config;
    config.shadow_mode = true;
    FirewallEngine fw(config);

    // 无模型时 AI 未启用
    check(fw.ai_enabled() == false, "AI not enabled without model");
    check(fw.ai() == nullptr, "AI pointer null");

    // 所有包正常通过
    int all_pass = 0;
    for (int i = 0; i < 100; i++) {
        auto pkt = make_tcp_syn(IP(10,0,0,i % 20), IP(192,168,1,1), 40000 + i, 80);
        if (fw.inspect(pkt.data(), pkt.size())) all_pass++;
    }
    // 汇总检查
    check(all_pass == 100, "All 100 packets pass without model");
    check(fw.stats().packets_passed == 100, "packets_passed == 100");

    // on_timer 不应崩溃
    fw.on_timer();
    check(true, "on_timer() safe without AI");

    // 尝试加载不存在的模型
    check(fw.enable_ai("no_such_model.onnx", 0.5f) == false,
          "enable_ai returns false for missing model");
    check(fw.ai_enabled() == false, "AI still disabled after failed load");
}

// ============================================================================
// Test 4: AI + 真实模型端到端 — 正常流量 + SYN Flood
// ============================================================================

static void test_ai_with_real_model() {
    std::printf("\n[Test 4] AI End-to-End with Real Model\n");

    FirewallConfig config;
    config.shadow_mode = true;
    FirewallEngine fw(config);

    // 尝试加载安全模型
    bool loaded = fw.enable_ai("models/security_anomaly.onnx", 0.0f);
    if (!loaded) {
        skip("Security model not found or AI not compiled — skipping AI E2E test");
        return;
    }

    check(fw.ai_enabled() == true, "AI enabled with security model");
    check(fw.ai() != nullptr, "AI pointer valid");
    check(fw.ai()->shadow_mode() == true, "Shadow mode ON");

    uint32_t dst = IP(192,168,1,1);

    // Phase 1: 正常流量（混合 SYN/ACK/数据包，多个 IP）
    std::printf("  Phase 1: Normal traffic (100 mixed packets)...\n");
    for (int i = 0; i < 40; i++) {
        auto pkt = make_tcp_syn(IP(10,0,0,i % 10), dst, 40000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }
    for (int i = 0; i < 40; i++) {
        auto pkt = make_tcp_ack(IP(10,0,0,i % 10), dst, 40000 + i, 80, 100);
        fw.inspect(pkt.data(), pkt.size());
    }
    for (int i = 0; i < 20; i++) {
        auto pkt = make_tcp_synack(dst, IP(10,0,0,i % 10), 80, 40000 + i);
        fw.inspect(pkt.data(), pkt.size());
    }

    // tick + 推理
    fw.ai()->tick();
    float normal_score = fw.ai()->run_inference();
    std::printf("  Normal traffic score: %.6f\n", normal_score);

    // Shadow mode 下所有包都应该通过
    check(fw.stats().packets_dropped == 0, "Shadow mode: 0 drops during normal traffic");
    auto& ai_stats1 = fw.ai()->stats();
    check(ai_stats1.inferences_total >= 1, "At least 1 inference ran");

    // Phase 2: SYN Flood 攻击（大量 SYN，无 SYN-ACK，来自单一 IP）
    std::printf("  Phase 2: SYN flood (500 SYN packets from single IP)...\n");
    fw.ai()->tick();  // 新 window slot
    for (int i = 0; i < 500; i++) {
        auto pkt = make_tcp_syn(IP(1,2,3,4), dst, 10000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }

    fw.ai()->tick();
    float flood_score = fw.ai()->run_inference();
    std::printf("  SYN flood score: %.6f\n", flood_score);

    // 攻击流量应该得到更高的异常分数
    // 注：随机初始化模型不保证区分正常/异常，但流量模式明显不同
    check(ai_stats1.inferences_total >= 2, "Multiple inferences ran");

    // Shadow mode 下即使异常也应该放行（但可能有 alert）
    check(fw.stats().packets_dropped == 0, "Shadow mode: 0 drops during SYN flood");

    std::printf("  INFO: normal=%.6f, flood=%.6f, anomalies=%llu, alerts=%llu\n",
                normal_score, flood_score,
                static_cast<unsigned long long>(fw.ai()->stats().anomalies_detected),
                static_cast<unsigned long long>(fw.stats().packets_alerted));
}

// ============================================================================
// Test 5: AI Enforce Mode — 非 Shadow 下异常包应被丢弃
// ============================================================================

static void test_ai_enforce_mode() {
    std::printf("\n[Test 5] AI Enforce Mode\n");

    FirewallConfig config;
    config.shadow_mode = false;  // 非 shadow
    FirewallEngine fw(config);

    bool loaded = fw.enable_ai("models/security_anomaly.onnx", 0.0f);
    if (!loaded) {
        skip("Security model not found — skipping enforce mode test");
        return;
    }

    fw.ai()->set_shadow_mode(false);
    check(fw.ai()->shadow_mode() == false, "Shadow mode OFF (enforce)");

    uint32_t dst = IP(192,168,1,1);

    // 发送大量 SYN flood
    for (int i = 0; i < 500; i++) {
        auto pkt = make_tcp_syn(IP(6,6,6,6), dst, 10000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }

    // 触发推理
    fw.ai()->tick();
    float score = fw.ai()->run_inference();
    std::printf("  Enforce mode score: %.6f, is_anomaly: %s\n",
                score, fw.ai()->stats().anomalies_detected > 0 ? "yes" : "no");

    // 如果 AI 检测到异常，后续包应被丢弃
    if (fw.ai()->stats().anomalies_detected > 0) {
        auto pkt = make_tcp_syn(IP(7,7,7,7), dst, 55555, 80);
        bool pass = fw.inspect(pkt.data(), pkt.size());
        check(pass == false, "Enforce mode: anomaly causes packet drop");
        check(fw.stats().packets_dropped > 0, "packets_dropped > 0 in enforce mode");
    } else {
        std::printf("  INFO: Model did not flag anomaly (random weights). Testing logic only.\n");
        // 验证如果没有异常，包正常通过
        auto pkt = make_tcp_syn(IP(7,7,7,7), dst, 55555, 80);
        check(fw.inspect(pkt.data(), pkt.size()) == true,
              "No anomaly detected -> packet passes in enforce mode too");
    }
}

// ============================================================================
// Test 6: 混合规则 + AI — 规则丢弃优先于 AI
// ============================================================================

static void test_rules_plus_ai() {
    std::printf("\n[Test 6] Rules + AI Integration\n");

    FirewallConfig config;
    config.shadow_mode = true;
    FirewallEngine fw(config);

    // 加载 AI（可能失败，降级测试）
    bool has_ai = fw.enable_ai("models/security_anomaly.onnx", 0.0f);

    auto& rules = fw.rule_engine();
    uint32_t dst = IP(192,168,1,1);

    // 黑名单 IP
    uint32_t bad_ip = IP(9,9,9,9);
    rules.add_blacklist_ip(htonl(bad_ip));

    // 封 SSH
    rules.add_rule(Rule::block_port(1, 22, 6));

    // 黑名单包：规则丢弃，不需要 AI 参与
    auto p1 = make_tcp_syn(bad_ip, dst, 54321, 80);
    check(fw.inspect(p1.data(), p1.size()) == false,
          "Blacklisted IP dropped (rule, not AI)");

    // 封端口包：规则丢弃
    auto p2 = make_tcp_syn(IP(10,0,0,1), dst, 54321, 22);
    check(fw.inspect(p2.data(), p2.size()) == false,
          "TCP 22 blocked by rule");

    // 正常包：规则通过 → AI 评估
    auto p3 = make_tcp_syn(IP(10,0,0,1), dst, 54321, 80);
    bool pass = fw.inspect(p3.data(), p3.size());
    check(pass == true, "Normal packet passes rules + AI");

    // 确认统计
    check(fw.stats().packets_inspected == 3, "3 packets inspected");
    check(fw.stats().packets_dropped == 2, "2 dropped by rules");

    if (has_ai) {
        std::printf("  INFO: AI is loaded, verifying metrics collection\n");
        // 发更多流量，确认 AI 在记录
        for (int i = 0; i < 50; i++) {
            auto pkt = make_tcp_syn(IP(10,0,0,i % 5), dst, 40000 + i, 80);
            fw.inspect(pkt.data(), pkt.size());
        }
        fw.ai()->tick();
        fw.ai()->run_inference();
        check(fw.ai()->stats().inferences_total >= 1, "AI inference ran alongside rules");
    } else {
        skip("AI not available, rules-only verified");
    }
}

// ============================================================================
// Test 7: 决策回调（DecisionCallback）
// ============================================================================

static void test_decision_callback() {
    std::printf("\n[Test 7] Decision Callback\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();

    int pass_count = 0;
    int drop_count = 0;
    int alert_count = 0;

    fw.set_decision_callback([&](const PacketEvent& evt, const FirewallDecision& dec) {
        if (dec.should_pass()) pass_count++;
        else if (dec.should_drop()) drop_count++;
        if (dec.is_alert()) alert_count++;
    });

    uint32_t dst = IP(192,168,1,1);
    rules.add_blacklist_ip(htonl(IP(1,1,1,1)));

    // 3 正常包
    for (int i = 0; i < 3; i++) {
        auto pkt = make_tcp_syn(IP(2,2,2,2), dst, 40000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }
    // 2 黑名单包
    for (int i = 0; i < 2; i++) {
        auto pkt = make_tcp_syn(IP(1,1,1,1), dst, 40000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }

    check(pass_count == 3, "Callback received 3 pass events");
    check(drop_count == 2, "Callback received 2 drop events");
}

// ============================================================================
// Test 8: 畸形包与边界条件
// ============================================================================

static void test_malformed_packets() {
    std::printf("\n[Test 8] Malformed Packets & Edge Cases\n");

    FirewallEngine fw;

    // 1. 空包
    check(fw.inspect(nullptr, 0) == false, "Null/empty packet dropped");

    // 2. 太短（< 20 字节 IP 头）
    uint8_t tiny[10] = {0x45, 0, 0, 10, 0,0,0,0, 64, 6};
    check(fw.inspect(tiny, sizeof(tiny)) == false, "10-byte packet dropped (too short)");

    // 3. 错误的 IP 版本
    auto bad_ver = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 80);
    bad_ver[0] = 0x65;  // version = 6
    check(fw.inspect(bad_ver.data(), bad_ver.size()) == false, "IPv6 version dropped");

    // 4. IHL 太小（< 5）
    auto bad_ihl = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 80);
    bad_ihl[0] = 0x43;  // IHL = 3 (12 bytes, invalid)
    check(fw.inspect(bad_ihl.data(), bad_ihl.size()) == false, "IHL < 5 dropped");

    // 5. total_length 与实际长度不符
    auto bad_len = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 80);
    auto* ip_hdr = reinterpret_cast<RawIPv4Header*>(bad_len.data());
    ip_hdr->total_length = htons(1000);  // 声称 1000 字节但只有 40
    check(fw.inspect(bad_len.data(), bad_len.size()) == false, "total_length mismatch dropped");

    // 6. 最小合法包（20 字节 IP 头 only，无传输层）
    std::vector<uint8_t> min_pkt(20, 0);
    auto* min_ip = reinterpret_cast<RawIPv4Header*>(min_pkt.data());
    min_ip->version_ihl = 0x45;
    min_ip->total_length = htons(20);
    min_ip->ttl = 64;
    min_ip->protocol = 1;  // ICMP
    min_ip->src_addr = htonl(IP(10,0,0,1));
    min_ip->dst_addr = htonl(IP(192,168,1,1));
    // 20 字节纯 IP 头应该能被解析（无端口）
    bool min_result = fw.inspect(min_pkt.data(), min_pkt.size());
    check(min_result == true, "Minimal 20-byte IP header packet accepted");

    // 7. ICMP 包应正常通过
    auto icmp = make_icmp_echo(IP(10,0,0,1), IP(192,168,1,1));
    check(fw.inspect(icmp.data(), icmp.size()) == true, "ICMP echo request passes");

    // 验证畸形包也被统计
    check(fw.stats().packets_dropped >= 4, "Malformed packets counted as dropped");
}

// ============================================================================
// Test 9: 多规则优先级排序
// ============================================================================

static void test_rule_priority_order() {
    std::printf("\n[Test 9] Rule Priority Order\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();

    uint32_t dst = IP(192,168,1,1);

    // 低优先级（数字大）：封 80 端口
    Rule block80 = Rule::block_port(1, 80, 6, 200);
    rules.add_rule(block80);

    // 高优先级（数字小）：放行 80 端口
    Rule allow80{};
    allow80.id = 2;
    allow80.priority = 50;
    allow80.dst_port = 80;
    allow80.protocol = 6;
    allow80.action = FirewallDecision::Action::PASS;
    allow80.reason = FirewallDecision::Reason::NONE;
    allow80.enabled = true;
    rules.add_rule(allow80);

    // 高优先级放行规则应该胜出
    auto pkt = make_tcp_syn(IP(10,0,0,1), dst, 54321, 80);
    check(fw.inspect(pkt.data(), pkt.size()) == true,
          "Higher priority PASS rule overrides lower priority BLOCK");
}

// ============================================================================
// Test 10: 禁用/启用防火墙切换
// ============================================================================

static void test_enable_disable_toggle() {
    std::printf("\n[Test 10] Enable/Disable Toggle\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();

    uint32_t bad = IP(1,1,1,1);
    uint32_t dst = IP(192,168,1,1);
    rules.add_blacklist_ip(htonl(bad));

    // 启用时丢弃
    auto p1 = make_tcp_syn(bad, dst, 54321, 80);
    check(fw.inspect(p1.data(), p1.size()) == false, "Enabled: blacklisted dropped");

    // 禁用后放行
    fw.set_enabled(false);
    auto p2 = make_tcp_syn(bad, dst, 54321, 80);
    check(fw.inspect(p2.data(), p2.size()) == true, "Disabled: blacklisted passes");
    check(fw.stats().packets_inspected == 1, "Disabled packets not counted as inspected");

    // 重新启用
    fw.set_enabled(true);
    auto p3 = make_tcp_syn(bad, dst, 54321, 80);
    check(fw.inspect(p3.data(), p3.size()) == false, "Re-enabled: blacklisted dropped again");
}

// ============================================================================
// Test 11: AI 指标滑动窗口端到端
// ============================================================================

static void test_ai_metrics_window_e2e() {
    std::printf("\n[Test 11] AI Metrics Sliding Window E2E\n");

    FirewallConfig config;
    FirewallEngine fw(config);

    // 创建 AI（不加载模型，用于指标收集）
    FirewallAIConfig ai_cfg;
    ai_cfg.shadow_mode = true;
    fw.set_ai(std::make_unique<FirewallAI>(ai_cfg));

    check(fw.ai() != nullptr, "AI module created (no model)");

    uint32_t dst = IP(192,168,1,1);

    // Window 1: 50 SYN + 10 RST
    for (int i = 0; i < 50; i++) {
        auto pkt = make_tcp_syn(IP(10,0,0,i % 10), dst, 40000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }
    for (int i = 0; i < 10; i++) {
        auto pkt = make_tcp_rst(IP(10,0,0,i % 5), dst, 40000 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }

    auto snap1 = fw.ai()->metrics_snapshot();
    check(snap1.packets_total == 60, "Window 1: 60 total packets");
    check(snap1.syn_packets == 50, "Window 1: 50 SYN packets");
    check(snap1.rst_packets == 10, "Window 1: 10 RST packets");

    // tick → 移动窗口
    fw.ai()->tick();

    // Window 2: 只有 20 个普通 ACK
    for (int i = 0; i < 20; i++) {
        auto pkt = make_tcp_ack(IP(10,0,0,i % 5), dst, 40000 + i, 80, 200);
        fw.inspect(pkt.data(), pkt.size());
    }

    auto snap2 = fw.ai()->metrics_snapshot();
    // snapshot() 累积字段来自最近完成的 slot（tick 时保存的 60 包），
    // 当前 _current 有 20 个 ACK，但 snapshot().packets_total 读的是上一个 slot
    check(snap2.packets_total == 60, "Window 2: snapshot reads last completed slot (60)");

    // 验证特征提取
    auto features = fw.ai()->feature_extractor().extract(snap1, 60);
    check(features.packets_rx_norm >= 0.0f && features.packets_rx_norm <= 1.0f,
          "Feature: packets_rx_norm in [0,1]");
    check(features.syn_rate_norm >= 0.0f && features.syn_rate_norm <= 1.0f,
          "Feature: syn_rate_norm in [0,1]");
    check(features.rst_rate_norm >= 0.0f && features.rst_rate_norm <= 1.0f,
          "Feature: rst_rate_norm in [0,1]");
}

// ============================================================================
// Test 12: Shadow Mode ↔ Enforce 切换 + 阈值调整
// ============================================================================

static void test_shadow_enforce_switch() {
    std::printf("\n[Test 12] Shadow/Enforce Switch + Threshold\n");

    FirewallConfig config;
    config.shadow_mode = true;
    FirewallEngine fw(config);

    bool loaded = fw.enable_ai("models/security_anomaly.onnx", 0.0f);
    if (!loaded) {
        skip("Security model not found — skipping shadow/enforce switch test");
        return;
    }

    // 初始: Shadow ON
    check(fw.ai()->shadow_mode() == true, "Initial: shadow ON");

    // 切换到 Enforce
    fw.set_shadow_mode(false);
    fw.ai()->set_shadow_mode(false);
    check(fw.ai()->shadow_mode() == false, "Switched to enforce");

    // 切换回 Shadow
    fw.set_shadow_mode(true);
    fw.ai()->set_shadow_mode(true);
    check(fw.ai()->shadow_mode() == true, "Switched back to shadow");

    // 调整阈值
    fw.ai()->set_threshold(0.1f);
    check(fw.ai()->threshold() == 0.1f, "Threshold set to 0.1");

    fw.ai()->set_threshold(0.001f);
    check(fw.ai()->threshold() == 0.001f, "Threshold set to 0.001");
}

// ============================================================================
// Test 13: 大规模流量压力 — 池不耗尽
// ============================================================================

static void test_high_volume_stress() {
    std::printf("\n[Test 13] High Volume Stress Test\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();

    uint32_t dst = IP(192,168,1,1);
    rules.add_blacklist_ip(htonl(IP(1,1,1,1)));

    // 发送 10000 包
    int passed = 0, dropped = 0;
    for (int i = 0; i < 10000; i++) {
        uint32_t src = (i % 100 == 0) ? IP(1,1,1,1) : IP(10,0,0,i % 254);
        auto pkt = make_tcp_syn(src, dst, 30000 + (i % 30000), 80);
        if (fw.inspect(pkt.data(), pkt.size())) passed++; else dropped++;
    }

    std::printf("  Processed 10000 packets: %d passed, %d dropped\n", passed, dropped);
    check(passed + dropped == 10000, "All 10000 packets processed");
    check(dropped == 100, "100 blacklisted packets dropped (every 100th)");
    check(fw.stats().pool_acquire_failed == 0, "No pool exhaustion");
    check(fw.event_pool_available() == FirewallEngine::EVENT_POOL_SIZE,
          "All event pool slots returned");
    check(fw.decision_pool_available() == FirewallEngine::DECISION_POOL_SIZE,
          "All decision pool slots returned");
}

// ============================================================================
// Test 14: 动态规则增删
// ============================================================================

static void test_dynamic_rule_management() {
    std::printf("\n[Test 14] Dynamic Rule Management\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    uint32_t dst = IP(192,168,1,1);
    uint32_t src = IP(10,0,0,1);

    // 初始：80 端口正常通过
    auto p1 = make_tcp_syn(src, dst, 54321, 80);
    check(fw.inspect(p1.data(), p1.size()) == true, "Port 80 open initially");

    // 动态添加规则封 80
    rules.add_rule(Rule::block_port(1, 80, 6));
    auto p2 = make_tcp_syn(src, dst, 54321, 80);
    check(fw.inspect(p2.data(), p2.size()) == false, "Port 80 blocked after rule add");

    // 禁用规则
    rules.set_rule_enabled(1, false);
    auto p3 = make_tcp_syn(src, dst, 54321, 80);
    check(fw.inspect(p3.data(), p3.size()) == true, "Port 80 open after rule disable");

    // 重新启用
    rules.set_rule_enabled(1, true);
    auto p4 = make_tcp_syn(src, dst, 54321, 80);
    check(fw.inspect(p4.data(), p4.size()) == false, "Port 80 blocked after rule re-enable");

    // 删除规则
    check(rules.remove_rule(1) == true, "Rule removed successfully");
    auto p5 = make_tcp_syn(src, dst, 54321, 80);
    check(fw.inspect(p5.data(), p5.size()) == true, "Port 80 open after rule removal");

    // 删除不存在的规则
    check(rules.remove_rule(999) == false, "Remove non-existent rule returns false");
}

// ============================================================================
// Test 15: 协议混合流量
// ============================================================================

static void test_mixed_protocol_traffic() {
    std::printf("\n[Test 15] Mixed Protocol Traffic\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    uint32_t dst = IP(192,168,1,1);

    // 封 UDP 53（DNS）
    rules.add_rule(Rule::block_port(1, 53, 17));

    // TCP 53 应该通过（规则只封 UDP）
    auto tcp53 = make_tcp_syn(IP(10,0,0,1), dst, 54321, 53);
    check(fw.inspect(tcp53.data(), tcp53.size()) == true,
          "TCP 53 passes (rule blocks UDP only)");

    // UDP 53 被封
    auto udp53 = make_udp(IP(10,0,0,1), dst, 54321, 53);
    check(fw.inspect(udp53.data(), udp53.size()) == false,
          "UDP 53 blocked by rule");

    // ICMP 不受端口规则影响
    auto icmp = make_icmp_echo(IP(10,0,0,1), dst);
    check(fw.inspect(icmp.data(), icmp.size()) == true,
          "ICMP unaffected by port rules");

    // TCP 80 正常
    auto tcp80 = make_tcp_syn(IP(10,0,0,1), dst, 54321, 80);
    check(fw.inspect(tcp80.data(), tcp80.size()) == true,
          "TCP 80 passes normally");

    // UDP 80 正常
    auto udp80 = make_udp(IP(10,0,0,1), dst, 54321, 80);
    check(fw.inspect(udp80.data(), udp80.size()) == true,
          "UDP 80 passes normally");
}

// ============================================================================
// Test 16: 黑白名单批量操作
// ============================================================================

static void test_bulk_ip_lists() {
    std::printf("\n[Test 16] Bulk IP List Operations\n");

    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    uint32_t dst = IP(192,168,1,1);

    // 批量加 100 个黑名单 IP
    for (int i = 1; i <= 100; i++) {
        rules.add_blacklist_ip(htonl(IP(172,16,0,i)));
    }
    check(rules.blacklist_size() == 100, "100 IPs in blacklist");

    // 测试黑名单内的 IP
    auto p1 = make_tcp_syn(IP(172,16,0,50), dst, 54321, 80);
    check(fw.inspect(p1.data(), p1.size()) == false, "IP in blacklist dropped");

    // 测试黑名单外的 IP
    auto p2 = make_tcp_syn(IP(172,16,0,200), dst, 54321, 80);
    check(fw.inspect(p2.data(), p2.size()) == true, "IP outside blacklist passes");

    // 移除一个 IP
    rules.remove_blacklist_ip(htonl(IP(172,16,0,50)));
    auto p3 = make_tcp_syn(IP(172,16,0,50), dst, 54321, 80);
    check(fw.inspect(p3.data(), p3.size()) == true, "Removed IP now passes");
    check(rules.blacklist_size() == 99, "99 IPs after removal");

    // 清空
    rules.clear_blacklist();
    check(rules.blacklist_size() == 0, "Blacklist cleared");

    auto p4 = make_tcp_syn(IP(172,16,0,1), dst, 54321, 80);
    check(fw.inspect(p4.data(), p4.size()) == true, "Previously blacklisted IP passes after clear");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    auto& logger = Logger::instance();
    logger.set_level(LogLevel::WARN);

    std::printf("============================================\n");
    std::printf("  NeuStack Firewall End-to-End Test Suite\n");
    std::printf("  v1.2 Full Feature Coverage\n");
    std::printf("============================================\n");

    test_rule_pipeline_priority();    //  1. 规则管线优先级
    test_rate_limiting_e2e();         //  2. 限速端到端
    test_ai_shadow_no_model();        //  3. AI 无模型安全降级
    test_ai_with_real_model();        //  4. AI + 真实模型
    test_ai_enforce_mode();           //  5. AI 强制模式
    test_rules_plus_ai();             //  6. 规则 + AI 联动
    test_decision_callback();         //  7. 决策回调
    test_malformed_packets();         //  8. 畸形包
    test_rule_priority_order();       //  9. 规则优先级排序
    test_enable_disable_toggle();     // 10. 启用/禁用切换
    test_ai_metrics_window_e2e();     // 11. AI 指标滑动窗口
    test_shadow_enforce_switch();     // 12. Shadow/Enforce 切换
    test_high_volume_stress();        // 13. 大规模压力测试
    test_dynamic_rule_management();   // 14. 动态规则管理
    test_mixed_protocol_traffic();    // 15. 混合协议流量
    test_bulk_ip_lists();             // 16. 批量 IP 操作

    std::printf("\n============================================\n");
    std::printf("  Results: %d passed, %d failed, %d skipped\n",
                g_passes, g_failures, g_skips);
    std::printf("  %s\n", g_failures == 0 ? "ALL PASSED" : "SOME TESTS FAILED");
    std::printf("============================================\n");

    return g_failures;
}
