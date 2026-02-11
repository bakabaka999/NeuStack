#include <catch2/catch_test_macros.hpp>
#include "neustack/firewall/rule_engine.hpp"
#include "neustack/firewall/packet_event.hpp"

using namespace neustack;

// Helper: 创建测试用的 PacketEvent
static PacketEvent make_event(uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t protocol) {
    PacketEvent evt{};
    evt.src_ip = src_ip;
    evt.dst_ip = dst_ip;
    evt.src_port = src_port;
    evt.dst_port = dst_port;
    evt.protocol = protocol;
    evt.timestamp_us = 0;
    evt.total_len = 100;
    return evt;
}

// Helper: IP 地址转换 (a.b.c.d -> uint32_t)
static constexpr uint32_t IP(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

TEST_CASE("RuleEngine: Default Pass", "[firewall][rule_engine]") {
    RuleEngine engine;
    
    auto evt = make_event(IP(192,168,1,100), IP(10,0,0,1), 12345, 80, 6);
    auto decision = engine.evaluate(evt, 0);
    
    CHECK(decision.action == FirewallDecision::Action::PASS);
    CHECK(decision.reason == FirewallDecision::Reason::NONE);
}

TEST_CASE("RuleEngine: Blacklist IP", "[firewall][rule_engine]") {
    RuleEngine engine;
    
    uint32_t bad_ip = IP(1,2,3,4);
    engine.add_blacklist_ip(bad_ip);
    
    SECTION("Blocked IP is dropped") {
        auto evt = make_event(bad_ip, IP(10,0,0,1), 12345, 80, 6);
        auto decision = engine.evaluate(evt, 0);
        
        CHECK(decision.action == FirewallDecision::Action::DROP);
        CHECK(decision.reason == FirewallDecision::Reason::RULE_BLACKLIST);
        CHECK(engine.stats().blacklist_hits == 1);
    }
    
    SECTION("Other IPs pass") {
        auto evt = make_event(IP(5,6,7,8), IP(10,0,0,1), 12345, 80, 6);
        auto decision = engine.evaluate(evt, 0);
        
        CHECK(decision.action == FirewallDecision::Action::PASS);
    }
}

TEST_CASE("RuleEngine: Whitelist IP", "[firewall][rule_engine]") {
    RuleEngine engine;
    
    uint32_t good_ip = IP(192,168,1,1);
    uint32_t bad_ip = IP(1,2,3,4);
    
    // 白名单优先级高于黑名单
    engine.add_whitelist_ip(good_ip);
    engine.add_blacklist_ip(good_ip);  // 即使同时在黑名单
    
    auto evt = make_event(good_ip, IP(10,0,0,1), 12345, 80, 6);
    auto decision = engine.evaluate(evt, 0);
    
    CHECK(decision.action == FirewallDecision::Action::PASS);
    CHECK(decision.reason == FirewallDecision::Reason::RULE_WHITELIST);
}

TEST_CASE("RuleEngine: Custom Rules", "[firewall][rule_engine]") {
    RuleEngine engine;
    
    // 封禁端口 22 (SSH)
    engine.add_rule(Rule::block_port(1, 22, 6));
    
    SECTION("SSH traffic blocked") {
        auto evt = make_event(IP(1,2,3,4), IP(10,0,0,1), 54321, 22, 6);
        auto decision = engine.evaluate(evt, 0);
        
        CHECK(decision.action == FirewallDecision::Action::DROP);
        CHECK(decision.reason == FirewallDecision::Reason::RULE_PORT);
        CHECK(decision.rule_id == 1);
    }
    
    SECTION("HTTP traffic allowed") {
        auto evt = make_event(IP(1,2,3,4), IP(10,0,0,1), 54321, 80, 6);
        auto decision = engine.evaluate(evt, 0);
        
        CHECK(decision.action == FirewallDecision::Action::PASS);
    }
}

TEST_CASE("RuleEngine: Rule Priority", "[firewall][rule_engine]") {
    RuleEngine engine;
    
    // 低优先级 (priority=200): 放行所有
    Rule allow_all{};
    allow_all.id = 1;
    allow_all.priority = 200;
    allow_all.action = FirewallDecision::Action::PASS;
    allow_all.reason = FirewallDecision::Reason::RULE_WHITELIST;
    allow_all.enabled = true;
    allow_all.src_ip = 0;
    allow_all.dst_ip = 0;
    allow_all.src_port = 0;
    allow_all.dst_port = 0;
    allow_all.protocol = 0;
    allow_all.src_mask = 0;
    allow_all.dst_mask = 0;
    engine.add_rule(allow_all);
    
    // 高优先级 (priority=50): 封禁特定 IP
    engine.add_rule(Rule::blacklist_ip(2, IP(10,0,0,99), 50));
    
    SECTION("High priority rule wins") {
        auto evt = make_event(IP(10,0,0,99), IP(192,168,1,1), 12345, 80, 6);
        auto decision = engine.evaluate(evt, 0);
        
        CHECK(decision.action == FirewallDecision::Action::DROP);
        CHECK(decision.rule_id == 2);
    }
    
    SECTION("Other IPs use low priority rule") {
        auto evt = make_event(IP(10,0,0,50), IP(192,168,1,1), 12345, 80, 6);
        auto decision = engine.evaluate(evt, 0);
        
        CHECK(decision.action == FirewallDecision::Action::PASS);
        CHECK(decision.rule_id == 1);
    }
}

TEST_CASE("RuleEngine: Rule Management", "[firewall][rule_engine]") {
    RuleEngine engine;
    
    engine.add_rule(Rule::block_port(1, 22, 6));
    engine.add_rule(Rule::block_port(2, 23, 6));  // Telnet
    CHECK(engine.rule_count() == 2);
    
    SECTION("Remove rule") {
        engine.remove_rule(1);
        CHECK(engine.rule_count() == 1);
        
        // Port 22 now allowed
        auto evt = make_event(IP(1,2,3,4), IP(10,0,0,1), 54321, 22, 6);
        auto decision = engine.evaluate(evt, 0);
        CHECK(decision.action == FirewallDecision::Action::PASS);
    }
    
    SECTION("Disable rule") {
        engine.set_rule_enabled(1, false);
        
        auto evt = make_event(IP(1,2,3,4), IP(10,0,0,1), 54321, 22, 6);
        auto decision = engine.evaluate(evt, 0);
        CHECK(decision.action == FirewallDecision::Action::PASS);
    }
    
    SECTION("Clear all") {
        engine.clear_rules();
        CHECK(engine.rule_count() == 0);
    }
}
