#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/tcp_seq.hpp"
#include <cstdint>

using namespace neustack;

TEST_CASE("TCP Sequence Number: Lesser-than and Lesser-equal", "[transport][tcp_seq]") {
    SECTION("Normal comparison without wraparound") {
        uint32_t a = 1000;
        uint32_t b = 2000;
        
        CHECK(seq_lt(a, b) == true);
        CHECK(seq_le(a, b) == true);
        CHECK(seq_le(a, a) == true);
        CHECK(seq_lt(a, a) == false);
    }

    SECTION("Comparison with wraparound") {
        uint32_t a = 0xFFFFFF00; // 接近最大值
        uint32_t b = 0x00000010; // 溢出回绕后的小值
        
        // 在 TCP 逻辑中，b 是在 a 之后产生的，所以 a < b
        CHECK(seq_lt(a, b) == true);
        CHECK(seq_le(a, b) == true);
        CHECK(seq_lt(b, a) == false);
    }

    SECTION("Half-space boundary (2^31 difference)") {
        uint32_t a = 0x00000000;
        uint32_t b = 0x80000000; // 恰好差 2^31

        // (int32_t)(a - b) = (int32_t)0x80000000 = INT32_MIN < 0, 所以 a < b
        // (int32_t)(b - a) = (int32_t)0x80000000 = INT32_MIN < 0, 所以 b < a 也为 true
        // 这是边界歧义：差值恰好 2^31 时 lt 两边都为 true，gt 两边都为 false
        CHECK(seq_lt(a, b) == true);
        CHECK(seq_lt(b, a) == true);   // 边界歧义
        CHECK(seq_gt(a, b) == false);
        CHECK(seq_gt(b, a) == false);  // 边界歧义
    }
}

TEST_CASE("TCP Sequence Number: Greater-than and Greater-equal", "[transport][tcp_seq]") {
    SECTION("Normal and wraparound checks") {
        uint32_t a = 0x00000010;
        uint32_t b = 0xFFFFFF00;

        CHECK(seq_gt(a, b) == true);
        CHECK(seq_ge(a, b) == true);
        CHECK(seq_ge(a, a) == true);
        CHECK(seq_gt(b, a) == false);
    }
}

TEST_CASE("TCP Sequence Number: Range check", "[transport][tcp_seq]") {
    SECTION("Normal range [start, end)") {
        uint32_t start = 100;
        uint32_t end = 200;

        CHECK(seq_in_range(100, start, end) == true);  // 包含边界
        CHECK(seq_in_range(150, start, end) == true);  // 范围内
        CHECK(seq_in_range(200, start, end) == false); // 左闭右开
        CHECK(seq_in_range(50, start, end) == false);
        CHECK(seq_in_range(250, start, end) == false);
    }

    SECTION("Wrapped range [start, end)") {
        // 范围从 0xFFFFFFF0 跨越到 0x00000020
        uint32_t start = 0xFFFFFFF0;
        uint32_t end = 0x00000020;

        CHECK(seq_in_range(0xFFFFFFF5, start, end) == true);
        CHECK(seq_in_range(0x00000000, start, end) == true);
        CHECK(seq_in_range(0x00000019, start, end) == true);
        
        CHECK(seq_in_range(0x00000020, start, end) == false);
        CHECK(seq_in_range(0x00000050, start, end) == false);
        CHECK(seq_in_range(0xFFFFFFE0, start, end) == false);
    }

    SECTION("Empty or single-point range") {
        uint32_t start = 500;
        uint32_t end = 500;

        // start == end 定义为空集，任何值都不在范围内
        CHECK(seq_in_range(500, start, end) == false);
        CHECK(seq_in_range(501, start, end) == false);
    }
}