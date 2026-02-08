#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/tcp_reno.hpp"
#include <cstdint>

using namespace neustack;

TEST_CASE("TCPReno: Initialization and Basic State", "[transport][reno]") {
    SECTION("Default constructor with standard MSS") {
        TCPReno reno(1460);
        CHECK(reno.cwnd() == 1460);
        CHECK(reno.ssthresh() == 65535);
        CHECK(reno.in_fast_recovery() == false);
    }

    SECTION("MSS floor constraint (RFC 879)") {
        TCPReno reno(100); // 传入过小的 MSS
        CHECK(reno.cwnd() == 536); // 应被修正为 536
    }
}

TEST_CASE("TCPReno: Slow Start and Congestion Avoidance", "[transport][reno]") {
    const uint32_t mss = 1000;
    TCPReno reno(mss);

    SECTION("Slow Start: Exponential growth") {
        // 初始 cwnd = 1000, ssthresh = 65535
        reno.on_ack(mss, 0); 
        CHECK(reno.cwnd() == 2000); // 每个 ACK 增加 1 MSS

        reno.on_ack(mss, 0);
        CHECK(reno.cwnd() == 3000);
    }

    SECTION("Transition to Congestion Avoidance") {
        // 手动通过超时降低 ssthresh 到 2000
        reno.on_ack(mss * 2, 0); // cwnd = 3000
        reno.on_timeout();       // ssthresh = max(3000/2, 2*1000) = 2000, cwnd = 1000
        
        REQUIRE(reno.ssthresh() == 2000);
        REQUIRE(reno.cwnd() == 1000);

        // 依然在慢启动 (1000 < 2000)
        reno.on_ack(mss, 0);
        CHECK(reno.cwnd() == 2000);

        // 进入拥塞避免 (cwnd >= ssthresh)
        // 增加量 = MSS * MSS / cwnd = 1000 * 1000 / 2000 = 500
        reno.on_ack(mss, 0);
        CHECK(reno.cwnd() == 2500);

        // 下一次增加量 = 1000 * 1000 / 2500 = 400
        reno.on_ack(mss, 0);
        CHECK(reno.cwnd() == 2900);
    }
}

TEST_CASE("TCPReno: Fast Retransmit and Recovery", "[transport][reno]") {
    const uint32_t mss = 1000;
    TCPReno reno(mss);

    // 模拟运行到 cwnd = 10000
    for(int i = 0; i < 9; ++i) reno.on_ack(mss, 0);
    REQUIRE(reno.cwnd() == 10000);

    SECTION("Entering Fast Recovery on 3 Duplicate ACKs") {
        // 发生丢包
        reno.on_loss(mss);

        // ssthresh = cwnd / 2 = 5000
        // cwnd = ssthresh + 3 * MSS = 5000 + 3000 = 8000
        CHECK(reno.ssthresh() == 5000);
        CHECK(reno.cwnd() == 8000);
        CHECK(reno.in_fast_recovery() == true);

        SECTION("Window inflation on further dup ACKs") {
            reno.on_dup_ack();
            CHECK(reno.cwnd() == 9000); // 每次 dup ACK 增加 1 MSS
            
            reno.on_dup_ack();
            CHECK(reno.cwnd() == 10000);
        }

        SECTION("Exit Fast Recovery on New ACK") {
            reno.on_ack(mss, 0);
            CHECK(reno.in_fast_recovery() == false);
            CHECK(reno.cwnd() == 5000); // 重置回 ssthresh
        }
    }
}

TEST_CASE("TCPReno: Timeout Handling", "[transport][reno]") {
    const uint32_t mss = 1000;
    TCPReno reno(mss);

    // 增加 cwnd 到 4000
    for(int i = 0; i < 3; ++i) reno.on_ack(mss, 0);
    
    SECTION("Basic Timeout") {
        reno.on_timeout();
        CHECK(reno.ssthresh() == 2000); // cwnd/2
        CHECK(reno.cwnd() == 1000);      // 1 MSS
        CHECK(reno.in_fast_recovery() == false);
    }

    SECTION("Timeout while in Fast Recovery") {
        reno.on_loss(mss); // 进入快速恢复
        REQUIRE(reno.in_fast_recovery() == true);

        reno.on_timeout();
        CHECK(reno.in_fast_recovery() == false);
        CHECK(reno.cwnd() == 1000);
    }

    SECTION("ssthresh floor boundary") {
        // 如果 cwnd 很小 (如 2000)，ssthresh 不能低于 2*MSS
        reno.on_timeout();
        CHECK(reno.ssthresh() == 2000); 
    }
}