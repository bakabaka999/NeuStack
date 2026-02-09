#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/transport/tcp_builder.hpp"
#include "neustack/transport/tcp_seq.hpp"
#include "neustack/net/ipv4.hpp" // 需要 IPv4Packet 定义
#include <vector>
#include <cstring>
#include <arpa/inet.h>

using namespace neustack;

// ============================================================================
// 测试辅助工具
// ============================================================================

// 默认测试 IP
static const uint32_t SRC_IP = 0x0A000001; // 10.0.0.1
static const uint32_t DST_IP = 0x0A000002; // 10.0.0.2

/**
 * @brief 使用 TCPBuilder 构造标准合法的 TCP 包
 */
std::vector<uint8_t> build_valid_tcp(uint8_t flags = TCPFlags::SYN, 
                                     const std::vector<uint8_t>& payload = {}) {
    TCPBuilder builder;
    builder.set_src_port(12345)
           .set_dst_port(80)
           .set_seq(1000)
           .set_ack(0)
           .set_flags(flags)
           .set_window(65535);

    if (!payload.empty()) {
        builder.set_payload(payload.data(), payload.size());
    }

    std::vector<uint8_t> buffer(1500); // 足够大的缓冲区
    ssize_t len = builder.build(buffer.data(), buffer.size());
    
    // 必须填充校验和，否则 parser 会直接拒绝
    TCPBuilder::fill_checksum(buffer.data(), len, SRC_IP, DST_IP);
    
    buffer.resize(len);
    return buffer;
}

/**
 * @brief 构造用于 Parse 的 IPv4Packet 包装器
 */
IPv4Packet wrap_in_ipv4(const std::vector<uint8_t>& tcp_data) {
    IPv4Packet pkt;
    pkt.src_addr = SRC_IP;
    pkt.dst_addr = DST_IP;
    pkt.protocol = 6; // TCP
    pkt.payload = tcp_data.data();
    pkt.payload_length = tcp_data.size();
    return pkt;
}

// ============================================================================
// 测试用例
// ============================================================================

TEST_CASE("TCP Edge: Truncated packets", "[transport][tcp][edge]") {
    
    SECTION("0 字节输入") {
        std::vector<uint8_t> empty;
        auto pkt = wrap_in_ipv4(empty);
        auto res = TCPParser::parse(pkt);
        CHECK_FALSE(res.has_value());
    }

    SECTION("19 字节 (差1字节不够最小头)") {
        auto buffer = build_valid_tcp();
        buffer.resize(19); // 强行截断
        
        auto pkt = wrap_in_ipv4(buffer);
        auto res = TCPParser::parse(pkt);
        CHECK_FALSE(res.has_value());
    }

    SECTION("恰好 20 字节最小合法头") {
        auto buffer = build_valid_tcp(); // 默认无 payload，正好 20 字节
        REQUIRE(buffer.size() == 20);

        auto pkt = wrap_in_ipv4(buffer);
        auto res = TCPParser::parse(pkt);
        REQUIRE(res.has_value());
        CHECK(res->data_offset == 20);  // 存储的是字节数 (5 * 4 = 20)
        CHECK(res->is_syn());
    }
}

TEST_CASE("TCP Edge: Invalid data_offset", "[transport][tcp][edge]") {
    
    SECTION("data_offset < 5 (header too small)") {
        auto buffer = build_valid_tcp();
        // 修改 Data Offset (offset 12, 高 4 位)
        // 原本是 0x50 (5 << 4)，改为 0x40 (4 << 4)
        // 注意：修改了头部，校验和会失效。
        // 为了验证 parser 是因为 offset 错误而不是校验和错误失败，
        // 我们应该重新计算校验和。
        
        TCPHeader* hdr = reinterpret_cast<TCPHeader*>(buffer.data());
        hdr->data_offset = (4 << 4); // 设置为 4
        
        // 重新计算校验和
        TCPBuilder::fill_checksum(buffer.data(), buffer.size(), SRC_IP, DST_IP);

        auto pkt = wrap_in_ipv4(buffer);
        auto res = TCPParser::parse(pkt);
        CHECK_FALSE(res.has_value());
    }

    SECTION("data_offset > actual length") {
        auto buffer = build_valid_tcp();
        // 设置 data_offset 为 15 (60 bytes)，但实际包长只有 20
        TCPHeader* hdr = reinterpret_cast<TCPHeader*>(buffer.data());
        hdr->data_offset = (15 << 4);

        TCPBuilder::fill_checksum(buffer.data(), buffer.size(), SRC_IP, DST_IP);

        auto pkt = wrap_in_ipv4(buffer);
        auto res = TCPParser::parse(pkt);
        CHECK_FALSE(res.has_value());
    }
}

TEST_CASE("TCP Edge: Invalid flag combinations", "[transport][tcp][edge]") {
    // 构造一些通常非法的组合，验证 parser 是否能容忍 (或者根据策略拒绝)
    // 目前实现通常比较宽容，只要格式对就能解析

    SECTION("SYN + FIN") {
        auto buffer = build_valid_tcp(TCPFlags::SYN | TCPFlags::FIN);
        auto pkt = wrap_in_ipv4(buffer);
        auto res = TCPParser::parse(pkt);
        
        REQUIRE(res.has_value());
        CHECK(res->is_syn());
        CHECK(res->is_fin());
    }

    SECTION("Flags = 0") {
        auto buffer = build_valid_tcp(0);
        auto pkt = wrap_in_ipv4(buffer);
        auto res = TCPParser::parse(pkt);
        
        REQUIRE(res.has_value());
        CHECK(res->flags == 0);
    }
}

TEST_CASE("TCP Edge: Checksum validation", "[transport][tcp][edge]") {
    
    SECTION("正确 checksum") {
        auto buffer = build_valid_tcp();
        auto pkt = wrap_in_ipv4(buffer);
        
        // 显式验证 verify_checksum 接口
        CHECK(TCPParser::verify_checksum(pkt));
        // 验证 parse 接口
        CHECK(TCPParser::parse(pkt).has_value());
    }

    SECTION("checksum 错误 (翻转一个 bit)") {
        auto buffer = build_valid_tcp();
        // 破坏 Seq Num 的一个字节
        buffer[4] ^= 0xFF; 
        
        auto pkt = wrap_in_ipv4(buffer);
        
        CHECK_FALSE(TCPParser::verify_checksum(pkt));
        CHECK_FALSE(TCPParser::parse(pkt).has_value());
    }
}

TEST_CASE("TCP Edge: Sequence number wraparound", "[transport][tcp][edge]") {
    // 测试 tcp_seq.hpp 中的独立比较函数
    // 包含头文件 neustack/transport/tcp_seq.hpp (假定已存在)
    
    // a < b ?
    SECTION("seq_lt: Normal") {
        CHECK(seq_lt(100, 200));
        CHECK_FALSE(seq_lt(200, 100));
    }

    SECTION("seq_lt: Wraparound") {
        // 0xFFFFFFFE (big) < 0x00000010 (small) ? YES
        uint32_t big = 0xFFFFFFFE;
        uint32_t small = 0x00000010;
        CHECK(seq_lt(big, small));
    }

    SECTION("seq_in_range: Wraparound") {
        // Range [MAX-10, 10)
        uint32_t start = UINT32_MAX - 10;
        uint32_t end = 10;
        
        CHECK(seq_in_range(UINT32_MAX, start, end)); // 在末尾
        CHECK(seq_in_range(0, start, end));          // 刚好回绕
        CHECK(seq_in_range(5, start, end));          // 回绕后
        CHECK_FALSE(seq_in_range(11, start, end));   // 超出
        CHECK_FALSE(seq_in_range(start - 1, start, end)); // 未到
    }
    
    SECTION("TCPSegment logic: seq_end with wrap") {
        TCPSegment seg;
        seg.seq_num = UINT32_MAX;
        seg.data_length = 0;
        seg.flags = TCPFlags::SYN; // len = 1
        
        // MAX + 1 -> 0
        CHECK(seg.seq_end() == 0);
    }
}