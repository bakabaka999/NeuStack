#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/tcp_builder.hpp"
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/net/ipv4.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
#include "neustack/common/platform.hpp" 

using namespace neustack;

TEST_CASE("TCPBuilder: Basic segment construction", "[transport][tcp_builder]") {
    TCPBuilder builder;
    uint8_t buffer[128];

    SECTION("Construct SYN packet and verify header offsets") {
        builder.set_src_port(12345)
               .set_dst_port(80)
               .set_seq(1000)
               .set_flags(TCPFlags::SYN)
               .set_window(8192);

        ssize_t len = builder.build(buffer, sizeof(buffer));
        // 修正：显式转换比较类型，处理 sign-compare
        CHECK(len == static_cast<ssize_t>(20)); 

        // 验证网络字节序偏移
        CHECK(*reinterpret_cast<uint16_t*>(buffer + 0) == htons(12345));
        CHECK(*reinterpret_cast<uint16_t*>(buffer + 2) == htons(80));
        CHECK(*reinterpret_cast<uint32_t*>(buffer + 4) == htonl(1000));
        CHECK(buffer[13] == TCPFlags::SYN);
        CHECK(*reinterpret_cast<uint16_t*>(buffer + 14) == htons(8192));
    }

    SECTION("Construct PSH|ACK packet with payload") {
        const uint8_t payload[] = "hello neustack";
        size_t payload_len = sizeof(payload);

        builder.set_src_port(443)
               .set_dst_port(54321)
               .set_seq(2000)
               .set_ack(5000)
               .set_flags(TCPFlags::PSH | TCPFlags::ACK)
               .set_payload(payload, payload_len);

        ssize_t len = builder.build(buffer, sizeof(buffer));
        // 修正：显式转换比较类型
        CHECK(len == static_cast<ssize_t>(20 + payload_len));

        CHECK(std::memcmp(buffer + 20, payload, payload_len) == 0);
    }
}

TEST_CASE("TCPBuilder: Integration with TCPParser and Checksum", "[transport][tcp_builder]") {
    TCPBuilder builder;
    uint8_t buffer[256];
    uint32_t src_ip = 0xC0A80001; // 192.168.0.1
    uint32_t dst_ip = 0xC0A80002; // 192.168.0.2

    SECTION("Round-trip: build -> fill_checksum -> parse") {
        const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
        builder.set_src_port(1024)
               .set_dst_port(2048)
               .set_seq(0x11223344)
               .set_ack(0x55667788)
               .set_flags(TCPFlags::ACK)
               .set_payload(data, sizeof(data));

        ssize_t len = builder.build(buffer, sizeof(buffer));
        REQUIRE(len > 0);

        TCPBuilder::fill_checksum(buffer, static_cast<size_t>(len), src_ip, dst_ip);
        
        uint16_t checksum_field = *reinterpret_cast<uint16_t*>(buffer + 16);
        CHECK(checksum_field != 0);

        IPv4Packet ip_pkt;
        ip_pkt.src_addr = src_ip;
        ip_pkt.dst_addr = dst_ip;
        ip_pkt.protocol = 6; 
        ip_pkt.payload = buffer;
        ip_pkt.payload_length = static_cast<size_t>(len);

        CHECK(TCPParser::verify_checksum(ip_pkt) == true);

        auto parsed_opt = TCPParser::parse(ip_pkt);
        REQUIRE(parsed_opt.has_value());
        
        const auto &seg = parsed_opt.value();
        CHECK(seg.src_port == 1024);
        CHECK(seg.dst_port == 2048);
        CHECK(seg.seq_num == 0x11223344);
        CHECK(seg.ack_num == 0x55667788);
        CHECK(seg.is_ack() == true);
        CHECK(std::memcmp(seg.data, data, sizeof(data)) == 0);
    }
}