/**
 * 集成测试：防火墙数据包过滤
 *
 * 验证 FirewallEngine 在真实数据包场景下的工作：
 * 1. 正常包放行
 * 2. 黑名单 IP 丢弃
 * 3. 封禁端口丢弃
 * 4. 限速触发丢弃
 */

#include "neustack/firewall.hpp"
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
    tcp->flags = 0x02;
    tcp->window = htons(65535);
    
    return packet;
}

static std::vector<uint8_t> make_udp(uint32_t src_ip, uint32_t dst_ip,
                                      uint16_t src_port, uint16_t dst_port) {
    std::vector<uint8_t> packet(28, 0);
    
    auto* ip = reinterpret_cast<RawIPv4Header*>(packet.data());
    ip->version_ihl = 0x45;
    ip->total_length = htons(28);
    ip->ttl = 64;
    ip->protocol = 17;
    ip->src_addr = htonl(src_ip);
    ip->dst_addr = htonl(dst_ip);
    
    auto* udp = reinterpret_cast<RawUDPHeader*>(packet.data() + 20);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(8);
    
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

static void test_default_pass() {
    std::printf("\n[Test] Default Pass\n");
    
    FirewallEngine fw;
    auto pkt = make_tcp_syn(IP(192,168,1,100), IP(10,0,0,1), 54321, 80);
    
    bool result = fw.inspect(pkt.data(), pkt.size());
    check(result == true, "Normal packet should pass");
    check(fw.stats().packets_passed == 1, "packets_passed == 1");
    check(fw.stats().packets_dropped == 0, "packets_dropped == 0");
}

static void test_blacklist_ip() {
    std::printf("\n[Test] Blacklist IP\n");
    
    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    
    uint32_t attacker = IP(1,2,3,4);
    rules.add_blacklist_ip(htonl(attacker));
    
    // 黑名单 IP 应该被丢弃
    auto pkt1 = make_tcp_syn(attacker, IP(10,0,0,1), 54321, 80);
    check(fw.inspect(pkt1.data(), pkt1.size()) == false, "Blacklisted IP dropped");
    
    // 其他 IP 应该通过
    auto pkt2 = make_tcp_syn(IP(5,6,7,8), IP(10,0,0,1), 54321, 80);
    check(fw.inspect(pkt2.data(), pkt2.size()) == true, "Other IP passes");
}

static void test_whitelist_priority() {
    std::printf("\n[Test] Whitelist Priority\n");
    
    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    
    uint32_t vip = IP(192,168,1,1);
    
    // 同时加入黑白名单
    rules.add_whitelist_ip(htonl(vip));
    rules.add_blacklist_ip(htonl(vip));
    
    // 白名单优先
    auto pkt = make_tcp_syn(vip, IP(10,0,0,1), 54321, 80);
    check(fw.inspect(pkt.data(), pkt.size()) == true, "Whitelist overrides blacklist");
}

static void test_port_block() {
    std::printf("\n[Test] Port Block Rule\n");
    
    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    
    // 封禁 SSH 端口 (TCP only)
    rules.add_rule(Rule::block_port(1, 22, 6));
    
    // TCP SSH 被封
    auto pkt1 = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 22);
    check(fw.inspect(pkt1.data(), pkt1.size()) == false, "TCP port 22 blocked");
    
    // HTTP 通过
    auto pkt2 = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 80);
    check(fw.inspect(pkt2.data(), pkt2.size()) == true, "TCP port 80 allowed");
    
    // UDP 22 通过 (规则只限 TCP)
    auto pkt3 = make_udp(IP(1,2,3,4), IP(10,0,0,1), 54321, 22);
    check(fw.inspect(pkt3.data(), pkt3.size()) == true, "UDP port 22 allowed (rule is TCP only)");
}

static void test_rate_limiting() {
    std::printf("\n[Test] Rate Limiting\n");
    
    FirewallEngine fw;
    auto& limiter = fw.rule_engine().rate_limiter();
    
    // 启用限速: 突发 3 个包
    limiter.set_enabled(true);
    limiter.set_rate(5, 3);
    
    uint32_t src = IP(10,0,0,50);
    int passed = 0, dropped = 0;
    
    // 发 5 个包，前 3 个应该通过
    for (int i = 0; i < 5; i++) {
        auto pkt = make_tcp_syn(src, IP(10,0,0,1), 54321 + i, 80);
        if (fw.inspect(pkt.data(), pkt.size())) {
            passed++;
        } else {
            dropped++;
        }
    }
    
    check(passed == 3, "Burst of 3 packets passed");
    check(dropped == 2, "Excess packets dropped");
}

static void test_disabled_firewall() {
    std::printf("\n[Test] Disabled Firewall\n");
    
    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    
    rules.add_blacklist_ip(htonl(IP(1,2,3,4)));
    fw.set_enabled(false);
    
    auto pkt = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 80);
    check(fw.inspect(pkt.data(), pkt.size()) == true, "Disabled firewall passes all");
    check(fw.stats().packets_inspected == 0, "No packets inspected when disabled");
}

static void test_malformed_packet() {
    std::printf("\n[Test] Malformed Packet\n");
    
    FirewallEngine fw;
    
    // 太短
    uint8_t tiny[10] = {0};
    check(fw.inspect(tiny, sizeof(tiny)) == false, "Too short packet dropped");
    
    // 错误的 IP 版本
    auto pkt = make_tcp_syn(IP(1,2,3,4), IP(10,0,0,1), 54321, 80);
    pkt[0] = 0x65;  // 改成 IPv6
    check(fw.inspect(pkt.data(), pkt.size()) == false, "Invalid IP version dropped");
}

static void test_stats_accuracy() {
    std::printf("\n[Test] Stats Accuracy\n");
    
    FirewallEngine fw;
    auto& rules = fw.rule_engine();
    
    rules.add_blacklist_ip(htonl(IP(1,1,1,1)));
    
    // 5 个正常包
    for (int i = 0; i < 5; i++) {
        auto pkt = make_tcp_syn(IP(2,2,2,2), IP(10,0,0,1), 54321 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }
    
    // 3 个黑名单包
    for (int i = 0; i < 3; i++) {
        auto pkt = make_tcp_syn(IP(1,1,1,1), IP(10,0,0,1), 54321 + i, 80);
        fw.inspect(pkt.data(), pkt.size());
    }
    
    check(fw.stats().packets_inspected == 8, "packets_inspected == 8");
    check(fw.stats().packets_passed == 5, "packets_passed == 5");
    check(fw.stats().packets_dropped == 3, "packets_dropped == 3");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    auto& logger = Logger::instance();
    logger.set_level(LogLevel::WARN);

    std::printf("=== Firewall Integration Test ===\n");

    test_default_pass();
    test_blacklist_ip();
    test_whitelist_priority();
    test_port_block();
    test_rate_limiting();
    test_disabled_firewall();
    test_malformed_packet();
    test_stats_accuracy();

    std::printf("\n=================================\n");
    std::printf("Passed: %d, Failed: %d\n", g_passes, g_failures);
    std::printf("%s\n", g_failures == 0 ? "=== ALL PASSED ===" : "=== SOME TESTS FAILED ===");

    return g_failures;
}
