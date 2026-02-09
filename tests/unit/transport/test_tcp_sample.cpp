#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "neustack/metrics/tcp_sample.hpp"

using namespace neustack;
using Catch::Matchers::WithinAbs;

TEST_CASE("TCPSample: Metrics calculation and validity", "[metrics][tcp_sample]") {
    // 使用大括号初始化确保 POD 字段清零
    TCPSample sample{};

    SECTION("Default values and safety") {
        // 验证初始状态下的计算安全性
        CHECK(sample.loss_rate() == 0.0f);       // 0/0 应返回 0.0f
        CHECK(sample.queuing_delay_us() == 0);   // min_rtt 为 0 时应返回 0
        CHECK(sample.rtt_ratio() == 1.0f);       // min_rtt 为 0 时应返回 1.0f
        CHECK(sample.is_valid_for_bw_estimation() == true); // 默认非 app limited 且无超时
    }

    SECTION("Loss rate calculation") {
        sample.packets_sent = 100;
        sample.packets_lost = 5;
        // 5 / 100 = 0.05
        CHECK_THAT(sample.loss_rate(), WithinAbs(0.05f, 0.0001f));

        // 边界：发送包数为 0
        sample.packets_sent = 0;
        CHECK(sample.loss_rate() == 0.0f);
    }

    SECTION("Queuing delay calculation") {
        sample.min_rtt_us = 10000; // 10ms 基线

        // 情况 1: 存在排队延迟
        sample.rtt_us = 15000;     // 15ms
        CHECK(sample.queuing_delay_us() == 5000); // 15ms - 10ms = 5ms

        // 情况 2: RTT 小于历史最小值（可能发生了路由优化或采样波动）
        sample.rtt_us = 8000;      // 8ms
        CHECK(sample.queuing_delay_us() == 0); // 应返回 0 而非负数溢出

        // 情况 3: 无基线数据
        sample.min_rtt_us = 0;
        CHECK(sample.queuing_delay_us() == 0);
    }

    SECTION("RTT ratio calculation") {
        sample.min_rtt_us = 10000;
        sample.rtt_us = 12000;
        // 12000 / 10000 = 1.2
        CHECK_THAT(sample.rtt_ratio(), WithinAbs(1.2f, 0.0001f));

        // 边界：基线为 0
        sample.min_rtt_us = 0;
        CHECK(sample.rtt_ratio() == 1.0f);
    }

    SECTION("Bandwidth estimation validity") {
        // 仅当非应用受限且未发生重传超时时有效
        sample.is_app_limited = 0;
        sample.timeout_occurred = 0;
        CHECK(sample.is_valid_for_bw_estimation() == true);

        // 应用受限（发送缓冲区空）
        sample.is_app_limited = 1;
        CHECK(sample.is_valid_for_bw_estimation() == false);

        // 恢复后发生超时
        sample.is_app_limited = 0;
        sample.timeout_occurred = 1;
        CHECK(sample.is_valid_for_bw_estimation() == false);
    }
}