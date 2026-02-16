#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/common/checksum.hpp"
#include <vector>
#include <optional>
#include <cstdint>
#include <cstring>
#include "neustack/common/platform.hpp"

using namespace neustack;

TEST_CASE("TCPSegment flag and length logic", "[transport][tcp]") {
    TCPSegment seg{};

    SECTION("Single flag: SYN") {
        seg.flags = 0x02;
        CHECK(seg.is_syn() == true);
        CHECK(seg.is_ack() == false);
        CHECK(seg.is_fin() == false);
        CHECK(seg.is_rst() == false);
        CHECK(seg.is_psh() == false);
    }

    SECTION("Flag combinations: SYN + ACK") {
        seg.flags = 0x12; // 0x10 (ACK) | 0x02 (SYN)
        CHECK(seg.is_syn() == true);
        CHECK(seg.is_ack() == true);
        CHECK(seg.is_fin() == false);
    }

    SECTION("Segment length calculation (seg_len)") {
        SECTION("SYN packet with no data") {
            seg.flags = 0x02;
            seg.data_length = 0;
            CHECK(seg.seg_len() == 1);
        }

        SECTION("Pure ACK packet with no data") {
            seg.flags = 0x10;
            seg.data_length = 0;
            CHECK(seg.seg_len() == 0);
        }

        SECTION("FIN packet with data") {
            seg.flags = 0x01;
            seg.data_length = 10;
            CHECK(seg.seg_len() == 11);
        }

        SECTION("SYN + FIN with data") {
            seg.flags = 0x03;
            seg.data_length = 5;
            CHECK(seg.seg_len() == 7);
        }
    }

    SECTION("Sequence end calculation (seq_end)") {
        seg.seq_num = 100;
        seg.flags = 0x02; // SYN
        seg.data_length = 0;
        CHECK(seg.seq_end() == 101);

        seg.seq_num = 1000;
        seg.flags = 0x10; // ACK
        seg.data_length = 50;
        CHECK(seg.seq_end() == 1050);
    }
}

TEST_CASE("TCPParser: Packet parsing and validation", "[transport][tcp]") {
    SECTION("Parse valid TCP SYN packet with correct checksum") {
        // Build TCP header (20 bytes) with placeholder checksum
        uint8_t tcp_raw[20];
        std::memset(tcp_raw, 0, sizeof(tcp_raw));
        // Src port: 12345 (0x3039)
        tcp_raw[0] = 0x30; tcp_raw[1] = 0x39;
        // Dst port: 80 (0x0050)
        tcp_raw[2] = 0x00; tcp_raw[3] = 0x50;
        // Seq: 1000
        tcp_raw[4] = 0x00; tcp_raw[5] = 0x00; tcp_raw[6] = 0x03; tcp_raw[7] = 0xE8;
        // Ack: 0
        // Data offset: 5 (20 bytes), flags: SYN (0x02)
        tcp_raw[12] = 0x50;
        tcp_raw[13] = 0x02;
        // Window: 28672 (0x7000)
        tcp_raw[14] = 0x70; tcp_raw[15] = 0x00;
        // Checksum: 0 (placeholder, will be computed)
        // Urgent: 0

        uint32_t src_ip = 0xC0A80101; // 192.168.1.1
        uint32_t dst_ip = 0xC0A80102; // 192.168.1.2

        // Build pseudo-header + TCP data for checksum, matching TCPParser::compute_tcp_checksum
        // TCPPseudoHeader: src_addr(4) dst_addr(4) zero(1) protocol(1) tcp_length(2) = 12 bytes
        uint8_t pseudo_buf[12 + 20];
        uint32_t src_net = htonl(src_ip);
        uint32_t dst_net = htonl(dst_ip);
        std::memcpy(pseudo_buf + 0, &src_net, 4);
        std::memcpy(pseudo_buf + 4, &dst_net, 4);
        pseudo_buf[8] = 0;
        pseudo_buf[9] = 6; // TCP
        uint16_t tcp_len_net = htons(20);
        std::memcpy(pseudo_buf + 10, &tcp_len_net, 2);
        std::memcpy(pseudo_buf + 12, tcp_raw, 20);

        uint16_t cksum = compute_checksum(pseudo_buf, sizeof(pseudo_buf));
        // compute_checksum returns htons(~sum), store directly in network byte order
        std::memcpy(tcp_raw + 16, &cksum, 2);

        IPv4Packet ip_pkt{};
        ip_pkt.src_addr = src_ip;
        ip_pkt.dst_addr = dst_ip;
        ip_pkt.protocol = 6;
        ip_pkt.payload = tcp_raw;
        ip_pkt.payload_length = 20;

        auto result = TCPParser::parse(ip_pkt);

        CHECK(result.has_value());
        if (result) {
            CHECK(result->src_port == 12345);
            CHECK(result->dst_port == 80);
            CHECK(result->seq_num == 1000);
            CHECK(result->is_syn() == true);
            CHECK(result->data_length == 0);
        }
    }

    SECTION("Malformed packet: Payload too short") {
        uint8_t short_data[] = { 0x01, 0x02 };
        IPv4Packet ip_pkt{};
        ip_pkt.protocol = 6;
        ip_pkt.payload = short_data;
        ip_pkt.payload_length = sizeof(short_data);

        auto result = TCPParser::parse(ip_pkt);
        CHECK_FALSE(result.has_value());
    }

    SECTION("Malformed packet: Data offset exceeds payload") {
        std::vector<uint8_t> tcp_raw(20, 0);
        tcp_raw[12] = 0x60; // Offset = 6 (24 bytes) but payload is only 20

        IPv4Packet ip_pkt{};
        ip_pkt.protocol = 6;
        ip_pkt.payload = tcp_raw.data();
        ip_pkt.payload_length = tcp_raw.size();

        auto result = TCPParser::parse(ip_pkt);
        CHECK_FALSE(result.has_value());
    }
}
