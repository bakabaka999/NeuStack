// tests/unit/hal/test_ethernet.cpp
// 测试 ethernet.hpp: EthernetHeader, parse_ethernet(), build_ethernet()

#include <catch2/catch_test_macros.hpp>
#include "neustack/hal/ethernet.hpp"
#include <cstring>
#include <vector>

using namespace neustack;

// ============================================================================
// EthernetHeader 结构体测试
// ============================================================================

TEST_CASE("EthernetHeader size is 14 bytes", "[hal][ethernet]") {
    CHECK(sizeof(EthernetHeader) == 14);
    CHECK(sizeof(EthernetHeader) == ETH_HLEN);
}

TEST_CASE("EthernetHeader ethertype conversion", "[hal][ethernet]") {
    EthernetHeader hdr{};

    SECTION("IPv4 (0x0800)") {
        // 网络字节序: 0x08, 0x00
        hdr.ethertype_be = 0x0008; // little-endian 存储的 0x0800
        CHECK(hdr.ethertype() == 0x0800);
    }

    SECTION("ARP (0x0806)") {
        hdr.ethertype_be = 0x0608;
        CHECK(hdr.ethertype() == 0x0806);
    }

    SECTION("IPv6 (0x86DD)") {
        hdr.ethertype_be = 0xDD86;
        CHECK(hdr.ethertype() == 0x86DD);
    }
}

// ============================================================================
// parse_ethernet 测试
// ============================================================================

TEST_CASE("parse_ethernet basic", "[hal][ethernet]") {
    // 构造一个最小的 Ethernet + IPv4 帧
    // dst(6) + src(6) + ethertype(2) + payload
    uint8_t frame[] = {
        // dst MAC
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        // src MAC
        0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e,
        // EtherType = 0x0800 (IPv4), 网络字节序
        0x08, 0x00,
        // Payload (fake IPv4 header start)
        0x45, 0x00, 0x00, 0x14
    };

    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    uint16_t etype = parse_ethernet(frame, sizeof(frame), &payload, &payload_len);

    CHECK(etype == ETH_P_IP);
    CHECK(payload == frame + 14);
    CHECK(payload_len == 4);
    CHECK(payload[0] == 0x45);
}

TEST_CASE("parse_ethernet too short", "[hal][ethernet]") {
    uint8_t frame[] = {0x00, 0x01, 0x02};

    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    uint16_t etype = parse_ethernet(frame, sizeof(frame), &payload, &payload_len);

    CHECK(etype == 0);
}

TEST_CASE("parse_ethernet exactly 14 bytes (no payload)", "[hal][ethernet]") {
    uint8_t frame[14] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x00
    };

    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    uint16_t etype = parse_ethernet(frame, sizeof(frame), &payload, &payload_len);

    CHECK(etype == ETH_P_IP);
    CHECK(payload_len == 0);
}

TEST_CASE("parse_ethernet ARP frame", "[hal][ethernet]") {
    uint8_t frame[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e,
        0x08, 0x06, // ARP
        0x00, 0x01  // ARP payload start
    };

    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    uint16_t etype = parse_ethernet(frame, sizeof(frame), &payload, &payload_len);

    CHECK(etype == ETH_P_ARP);
    CHECK(payload_len == 2);
}

TEST_CASE("parse_ethernet IPv6 frame", "[hal][ethernet]") {
    uint8_t frame[] = {
        0x33, 0x33, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e,
        0x86, 0xDD, // IPv6
        0x60, 0x00  // IPv6 header start
    };

    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    uint16_t etype = parse_ethernet(frame, sizeof(frame), &payload, &payload_len);

    CHECK(etype == ETH_P_IPV6);
    CHECK(payload_len == 2);
}

// ============================================================================
// build_ethernet 测试
// ============================================================================

TEST_CASE("build_ethernet writes correct header", "[hal][ethernet]") {
    // 分配 buffer: ETH_HLEN headroom + payload
    uint8_t buf[14 + 20] = {};
    uint8_t* l3_start = buf + ETH_HLEN;

    // 写一些 L3 数据
    l3_start[0] = 0x45;
    l3_start[1] = 0x00;

    uint8_t dst[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t src[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    uint8_t* frame_start = build_ethernet(l3_start, dst, src, ETH_P_IP);

    CHECK(frame_start == buf);

    // 验证 dst MAC
    CHECK(std::memcmp(frame_start, dst, 6) == 0);
    // 验证 src MAC
    CHECK(std::memcmp(frame_start + 6, src, 6) == 0);
    // 验证 EtherType (网络字节序 0x0800)
    CHECK(frame_start[12] == 0x08);
    CHECK(frame_start[13] == 0x00);
    // L3 数据没被破坏
    CHECK(frame_start[14] == 0x45);
    CHECK(frame_start[15] == 0x00);
}

TEST_CASE("build_ethernet ARP ethertype", "[hal][ethernet]") {
    uint8_t buf[14 + 4] = {};
    uint8_t* l3_start = buf + ETH_HLEN;
    uint8_t dst[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t src[6] = {0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e};

    build_ethernet(l3_start, dst, src, ETH_P_ARP);

    CHECK(buf[12] == 0x08);
    CHECK(buf[13] == 0x06);
}

// ============================================================================
// parse + build 往返测试
// ============================================================================

TEST_CASE("build then parse roundtrip", "[hal][ethernet]") {
    uint8_t buf[14 + 100] = {};
    uint8_t* l3_start = buf + ETH_HLEN;

    // 写 payload
    for (int i = 0; i < 100; ++i) l3_start[i] = static_cast<uint8_t>(i);

    uint8_t dst[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t src[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    build_ethernet(l3_start, dst, src, ETH_P_IP);

    // 现在 parse 回来
    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
    uint16_t etype = parse_ethernet(buf, sizeof(buf), &payload, &payload_len);

    CHECK(etype == ETH_P_IP);
    CHECK(payload_len == 100);
    CHECK(payload == l3_start);

    // 验证 payload 内容没变
    for (int i = 0; i < 100; ++i) {
        CHECK(payload[i] == static_cast<uint8_t>(i));
    }

    // 验证 MAC 可以从 header 读回来
    auto* hdr = reinterpret_cast<const EthernetHeader*>(buf);
    CHECK(std::memcmp(hdr->dst, dst, 6) == 0);
    CHECK(std::memcmp(hdr->src, src, 6) == 0);
}
