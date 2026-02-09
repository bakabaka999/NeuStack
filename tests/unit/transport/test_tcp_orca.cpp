#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "neustack/transport/tcp_orca.hpp"
#include <cmath>

using namespace neustack;
using Catch::Matchers::WithinAbs;

TEST_CASE("TCPOrca: Alpha modulation logic", "[transport][orca]") {
    const uint32_t mss = 1460;
    TCPOrca orca(mss);
    
    // 获取初始状态下的 CUBIC 窗口（通常为 1 MSS，但 Orca 最小约束为 2 MSS）
    uint32_t base_cwnd = orca.cwnd_cubic();

    SECTION("Neutral alpha (alpha = 0)") {
        orca.set_alpha(0.0f);
        // 2^0 = 1, cwnd == cwnd_cubic (受限于 MIN_CWND_MSS)
        CHECK(orca.cwnd() == std::max(base_cwnd, 2u * mss));
    }

    SECTION("Positive alpha (alpha = 1)") {
        orca.set_alpha(1.0f);
        // 2^1 = 2, cwnd == 2 * cwnd_cubic
        // 注意：底层 CUBIC 初始可能是 1 MSS，Orca 强制最小 2 MSS
        // 如果 base 是 1460，2^1 * 1460 = 2920
        CHECK(orca.cwnd() == base_cwnd * 2);
    }

    SECTION("Negative alpha (alpha = -1)") {
        // 先增加窗口，避免撞到 MIN_CWND_MSS 下限
        // 模拟多次 ACK 让 cubic 增长
        for(int i = 0; i < 10; ++i) orca.on_ack(mss * 10, 10000);
        
        uint32_t enlarged_cubic = orca.cwnd_cubic();
        REQUIRE(enlarged_cubic > 4 * mss);

        orca.set_alpha(-1.0f);
        // 2^-1 = 0.5, cwnd == 0.5 * cwnd_cubic
        CHECK(orca.cwnd() == enlarged_cubic / 2);
    }

    SECTION("Minimum CWND boundary") {
        // CUBIC 初始 cwnd = 10*MSS = 14600
        // alpha=-1 → 2^(-1) * 14600 = 7300, 仍然 > MIN_CWND_MSS * mss
        // 要触发下限，需要 cwnd_cubic 很小：先 timeout 把 cubic 降到最低
        orca.on_timeout();
        orca.set_alpha(-10.0f); // 会被 clamp 到 -1
        // 即使 alpha=-1: 2^(-1) * cubic_after_timeout
        // 如果 cubic_after_timeout 很小，结果可能触底到 MIN_CWND_MSS * mss
        uint32_t result = orca.cwnd();
        CHECK(result >= 2 * mss);  // 最小值保证
    }
}

TEST_CASE("TCPOrca: CUBIC delegation and state forwarding", "[transport][orca]") {
    const uint32_t mss = 1460;
    TCPOrca orca(mss);

    SECTION("on_ack updates underlying cubic and RTT") {
        uint32_t initial_cubic = orca.cwnd_cubic();
        orca.on_ack(mss, 20000); // 20ms RTT
        
        CHECK(orca.cwnd_cubic() > initial_cubic);
        CHECK(orca.rtt_us() == 20000);
        CHECK(orca.min_rtt_us() == 20000);

        orca.on_ack(mss, 15000); // 更小的 RTT
        CHECK(orca.min_rtt_us() == 15000);
    }

    SECTION("on_loss triggers cubic reduction") {
        // 先跑一段时间
        for(int i = 0; i < 20; ++i) orca.on_ack(mss, 10000);
        uint32_t before_loss = orca.cwnd_cubic();

        orca.on_loss(mss);
        CHECK(orca.cwnd_cubic() < before_loss);
    }
}

TEST_CASE("TCPOrca: Statistics and Metrics", "[transport][orca]") {
    const uint32_t mss = 1460;
    TCPOrca orca(mss);

    SECTION("Loss rate calculation") {
        orca.on_send(mss);
        orca.on_send(mss);
        orca.on_send(mss);
        orca.on_send(mss); // 发送 4 包
        
        orca.on_loss(mss); // 丢失 1 包
        
        CHECK_THAT(orca.loss_rate(), WithinAbs(0.25f, 0.0001f));

        orca.reset_period_stats();
        CHECK(orca.loss_rate() == 0.0f);
    }

    SECTION("Bandwidth and Delivery rate tracking") {
        orca.set_predicted_bandwidth(10'000'000); // 10Mbps
        orca.set_delivery_rate(8'000'000);        // 8Mbps
        
        CHECK(orca.predicted_bw() == 10'000'000);
        CHECK(orca.throughput() == 8'000'000);
    }
}

TEST_CASE("TCPOrca: Alpha Callback Integration", "[transport][orca]") {
    const uint32_t mss = 1460;
    bool callback_called = false;

    // 定义一个总是返回 0.5 的回调
    TCPOrca::AlphaCallback my_cb = [&](float throughput, float delay, float rtt_ratio, 
                                       float loss, float cwnd_n, float inflight, float pred) {
        callback_called = true;
        return 0.5f; 
    };

    TCPOrca orca(mss, my_cb);

    SECTION("Callback is triggered during on_ack") {
        orca.on_ack(mss, 10000);
        CHECK(callback_called == true);
        
        // 验证调制是否生效: 2^0.5 ≈ 1.414
        float multiplier = std::pow(2.0f, 0.5f);
        uint32_t expected = static_cast<uint32_t>(orca.cwnd_cubic() * multiplier);
        CHECK(orca.cwnd() == expected);
    }
}