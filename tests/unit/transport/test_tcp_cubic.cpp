#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/tcp_cubic.hpp"

using namespace neustack;

static constexpr uint32_t MSS = 1460;

TEST_CASE("TCPCubic: Initialization and Slow Start", "[transport][cubic]") {
    TCPCubic cubic(MSS);

    SECTION("Initial state") {
        CHECK(cubic.cwnd() == 10 * MSS);
        CHECK(cubic.ssthresh() == 65535);
        CHECK(cubic.in_slow_start() == true);
        CHECK(cubic.w_max() == 0.0);
    }

    SECTION("Slow start exponential growth") {
        uint32_t initial = cubic.cwnd();
        // 每个 ACK 增加 bytes_acked
        cubic.on_ack(MSS, 20000);
        CHECK(cubic.cwnd() == initial + MSS);

        cubic.on_ack(MSS, 20000);
        CHECK(cubic.cwnd() == initial + 2 * MSS);
    }

    SECTION("Slow start exits when cwnd reaches ssthresh") {
        // 设定一个较低的 ssthresh：先 loss 再 ack
        // 初始 cwnd=14600, loss 后 cwnd = 14600*0.7 = 10220, ssthresh=10220
        cubic.on_loss(0);
        CHECK(cubic.in_slow_start() == false);
        uint32_t ss = cubic.ssthresh();
        CHECK(cubic.cwnd() == ss);
    }
}

TEST_CASE("TCPCubic: Loss Response", "[transport][cubic]") {
    TCPCubic cubic(MSS);

    SECTION("Multiplicative decrease on loss") {
        uint32_t before = cubic.cwnd();
        cubic.on_loss(0);

        // cwnd 应减小到 beta * before
        uint32_t expected_ss = static_cast<uint32_t>(before * 0.7);
        CHECK(cubic.cwnd() == expected_ss);
        CHECK(cubic.ssthresh() == expected_ss);
        CHECK(cubic.in_slow_start() == false);
    }

    SECTION("W_max tracks pre-loss cwnd") {
        uint32_t before = cubic.cwnd();
        cubic.on_loss(0);
        // 首次 loss: w_max == 0 < cwnd, 所以 w_max = cwnd (before)
        CHECK(cubic.w_max() == static_cast<double>(before));
    }

    SECTION("Fast convergence: consecutive losses reduce w_max") {
        // 第一次 loss
        cubic.on_loss(0);
        double w_max_1 = cubic.w_max();

        // cwnd 现在小于 w_max_1，再次 loss 触发 fast convergence
        // w_max = cwnd * (1 + beta) / 2
        uint32_t cwnd_before_2nd = cubic.cwnd();
        cubic.on_loss(0);
        double expected_w_max = cwnd_before_2nd * (1 + 0.7) / 2;
        CHECK(cubic.w_max() == expected_w_max);
    }

    SECTION("ssthresh has minimum of MIN_CWND * MSS") {
        // 多次 loss 把 cwnd 压到很低
        for (int i = 0; i < 20; i++) {
            cubic.on_loss(0);
        }
        CHECK(cubic.ssthresh() >= 2 * MSS);
        CHECK(cubic.cwnd() >= 2 * MSS);
    }
}

TEST_CASE("TCPCubic: Timeout Handling", "[transport][cubic]") {
    TCPCubic cubic(MSS);

    SECTION("Timeout resets to 1 MSS and re-enters slow start") {
        // 先确认不在最低
        CHECK(cubic.cwnd() == 10 * MSS);

        cubic.on_timeout();
        CHECK(cubic.cwnd() == MSS);
        CHECK(cubic.in_slow_start() == true);
        // ssthresh = old_cwnd * beta
        CHECK(cubic.ssthresh() == static_cast<uint32_t>(10 * MSS * 0.7));
    }

    SECTION("Timeout ssthresh has minimum") {
        // 先把 cwnd 压到很低
        for (int i = 0; i < 20; i++) {
            cubic.on_loss(0);
        }
        cubic.on_timeout();
        CHECK(cubic.ssthresh() >= 2 * MSS);
        CHECK(cubic.cwnd() == MSS);
    }
}

TEST_CASE("TCPCubic: Congestion Avoidance Growth", "[transport][cubic]") {
    TCPCubic cubic(MSS);

    // 触发 loss 进入拥塞避免
    cubic.on_loss(0);
    uint32_t cwnd_after_loss = cubic.cwnd();

    SECTION("Growth in congestion avoidance is slower than slow start") {
        // 在拥塞避免阶段，每个 ACK 增长应比慢启动（+MSS）小
        uint32_t before = cubic.cwnd();
        cubic.on_ack(MSS, 20000);
        uint32_t growth = cubic.cwnd() - before;

        // 拥塞避免的增长应 <= MSS（不是指数增长了）
        CHECK(growth <= MSS);
    }

    SECTION("CWND grows over multiple ACKs") {
        uint32_t before = cubic.cwnd();
        for (int i = 0; i < 100; i++) {
            cubic.on_ack(MSS, 20000);
        }
        CHECK(cubic.cwnd() > before);
    }

    SECTION("CWND upper bound") {
        // 大量 ACK 不应超过上限 65535*16
        for (int i = 0; i < 10000; i++) {
            cubic.on_ack(MSS, 20000);
        }
        CHECK(cubic.cwnd() <= 65535u * 16);
    }
}
