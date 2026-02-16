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

TEST_CASE("RateLimiter: LRU Eviction", "[firewall][rate_limiter]") {
    RateLimiter::Config config;
    config.enabled = true;
    config.packets_per_second = 1000;
    config.burst_size = 100;

    RateLimiter limiter(config);

    // 填满到 MAX_TRACKED_IPS
    for (uint32_t i = 0; i < RateLimiter::MAX_TRACKED_IPS; i++) {
        limiter.check(i, i);  // 每个 IP 在不同时间创建
    }
    CHECK(limiter.tracked_count() == RateLimiter::MAX_TRACKED_IPS);

    // 再加一个 IP，应该触发驱逐 (1/4)
    uint32_t new_ip = RateLimiter::MAX_TRACKED_IPS + 1;
    limiter.check(new_ip, RateLimiter::MAX_TRACKED_IPS + 1);

    // 应该驱逐了 ~16384 个，加上新的 1 个
    size_t expected = RateLimiter::MAX_TRACKED_IPS - (RateLimiter::MAX_TRACKED_IPS / 4) + 1;
    CHECK(limiter.tracked_count() == expected);
}

TEST_CASE("RateLimiter: LRU promotes active IPs", "[firewall][rate_limiter]") {
    RateLimiter::Config config;
    config.enabled = true;
    config.packets_per_second = 1000;
    config.burst_size = 100;

    RateLimiter limiter(config);

    // 创建 IP 0 (最早)
    limiter.check(0, 0);
    // 创建 IP 1 到 MAX-1
    for (uint32_t i = 1; i < RateLimiter::MAX_TRACKED_IPS; i++) {
        limiter.check(i, i);
    }

    // 访问 IP 0 (提升到 LRU 尾部，不再是最旧)
    limiter.check(0, RateLimiter::MAX_TRACKED_IPS);

    // 触发驱逐
    uint32_t new_ip = RateLimiter::MAX_TRACKED_IPS + 1;
    limiter.check(new_ip, RateLimiter::MAX_TRACKED_IPS + 1);

    // IP 0 应该还在 (因为刚被访问过)，IP 1 应该被驱逐了 (最久未访问)
    // 验证：再次 check IP 0，应该还有历史 tokens（不是新桶的满 burst）
    // IP 1 如果存在会有历史 tokens，如果被驱逐则是新桶
    auto r0 = limiter.check(0, RateLimiter::MAX_TRACKED_IPS + 2);
    CHECK(r0.allowed == true);  // IP 0 存活

    // IP 1 被驱逐后重建，应该有满 burst 的 tokens
    auto r1 = limiter.check(1, RateLimiter::MAX_TRACKED_IPS + 3);
    CHECK(r1.tokens_left == config.burst_size - 1);  // 新桶: burst - 1 (刚消耗了一个)
}

TEST_CASE("RateLimiter: EWMA PPS smoothing", "[firewall][rate_limiter]") {
    RateLimiter::Config config;
    config.enabled = true;
    config.packets_per_second = 10000;
    config.burst_size = 10000;

    RateLimiter limiter(config);
    uint32_t ip = 0x01020304;

    // 每 1000us (1ms) 发一个包 → 1000 PPS
    uint64_t now = 0;
    for (int i = 0; i < 100; i++) {
        now += 1000;
        limiter.check(ip, now);
    }

    // 经过足够多次迭代，EWMA 应该接近 1000 PPS
    auto result = limiter.check(ip, now + 1000);
    CHECK(result.current_pps >= 800);
    CHECK(result.current_pps <= 1200);

    // 突然一个包间隔极短 (1us) → 不应该让 PPS 爆炸
    auto spike = limiter.check(ip, now + 1001);
    // EWMA 应该被平滑，不会瞬间飙到 1000000
    CHECK(spike.current_pps < 200000);
}
