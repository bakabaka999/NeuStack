#include <catch2/catch_test_macros.hpp>
#include "neustack/firewall/rate_limiter.hpp"

using namespace neustack;

TEST_CASE("RateLimiter: Disabled by default", "[firewall][rate_limiter]") {
    RateLimiter limiter;
    
    CHECK(limiter.enabled() == false);
    
    // 即使不启用，也应该返回 allowed
    auto result = limiter.check(0x01020304, 0);
    CHECK(result.allowed == true);
}

TEST_CASE("RateLimiter: Basic Rate Limiting", "[firewall][rate_limiter]") {
    RateLimiter::Config config;
    config.enabled = true;
    config.packets_per_second = 10;  // 每秒 10 个包
    config.burst_size = 5;           // 突发容量 5 个包
    
    RateLimiter limiter(config);
    
    uint32_t ip = 0x01020304;  // 1.2.3.4
    uint64_t now = 0;
    
    SECTION("Burst allowed") {
        // 前 5 个包应该立即放行 (突发容量)
        for (int i = 0; i < 5; i++) {
            auto result = limiter.check(ip, now);
            CHECK(result.allowed == true);
        }
        
        // 第 6 个包应该被限速
        auto result = limiter.check(ip, now);
        CHECK(result.allowed == false);
    }
    
    SECTION("Tokens refill over time") {
        // 消耗所有令牌
        for (int i = 0; i < 5; i++) {
            limiter.check(ip, now);
        }
        CHECK(limiter.check(ip, now).allowed == false);
        
        // 等待 500ms (应该补充 5 个令牌)
        now += 500000;  // 500ms in microseconds
        auto result = limiter.check(ip, now);
        CHECK(result.allowed == true);
    }
}

TEST_CASE("RateLimiter: Per-IP Tracking", "[firewall][rate_limiter]") {
    RateLimiter::Config config;
    config.enabled = true;
    config.packets_per_second = 10;
    config.burst_size = 2;
    
    RateLimiter limiter(config);
    
    uint32_t ip1 = 0x01020304;
    uint32_t ip2 = 0x05060708;
    uint64_t now = 0;
    
    // IP1 消耗完令牌
    limiter.check(ip1, now);
    limiter.check(ip1, now);
    CHECK(limiter.check(ip1, now).allowed == false);
    
    // IP2 应该有自己的桶，不受影响
    CHECK(limiter.check(ip2, now).allowed == true);
    CHECK(limiter.check(ip2, now).allowed == true);
    CHECK(limiter.check(ip2, now).allowed == false);
}

TEST_CASE("RateLimiter: Reset and Clear", "[firewall][rate_limiter]") {
    RateLimiter::Config config;
    config.enabled = true;
    config.packets_per_second = 10;
    config.burst_size = 2;
    
    RateLimiter limiter(config);
    
    uint32_t ip = 0x01020304;
    uint64_t now = 0;
    
    // 消耗令牌
    limiter.check(ip, now);
    limiter.check(ip, now);
    CHECK(limiter.check(ip, now).allowed == false);
    
    SECTION("Reset specific IP") {
        limiter.reset(ip);
        // 重置后应该有新的突发容量
        CHECK(limiter.check(ip, now).allowed == true);
    }
    
    SECTION("Clear all") {
        limiter.clear();
        CHECK(limiter.tracked_count() == 0);
        CHECK(limiter.check(ip, now).allowed == true);
    }
}
