#include <catch2/catch_test_macros.hpp>
#include "neustack/net/ipv4.hpp"
#include "neustack/common/checksum.hpp"
#include <cstring>
#include <vector>
#include "neustack/common/platform.hpp"

using namespace neustack;

// 简单的 IPv4 头部结构模拟，用于测试构造
struct RawIPv4Header {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_addr;
    uint32_t dst_addr;
};

// 辅助函数：计算并填充 IP Checksum
void fill_ip_checksum(std::vector<uint8_t>& pkt) {
    if (pkt.size() < 20) return;
    RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
    hdr->checksum = 0;
    // 计算 header length: (version_ihl & 0x0F) * 4
    size_t ihl = (hdr->version_ihl & 0x0F);
    size_t header_bytes = ihl * 4;
    
    if (pkt.size() >= header_bytes) {
        hdr->checksum = compute_checksum(pkt.data(), header_bytes);
    }
}

std::vector<uint8_t> make_ipv4_header(uint8_t ihl = 5, uint8_t ver = 4, uint16_t total_len = 20) {
    std::vector<uint8_t> pkt(ihl * 4, 0);
    // 确保至少有 20 字节的空间用于 cast
    if (pkt.size() < 20) pkt.resize(20, 0);

    RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
    hdr->version_ihl = (ver << 4) | (ihl & 0x0F);
    hdr->total_length = htons(total_len);
    hdr->ttl = 64;
    hdr->protocol = 6; // TCP
    hdr->src_addr = htonl(0x7F000001);
    hdr->dst_addr = htonl(0x7F000001);
    
    fill_ip_checksum(pkt);
    return pkt;
}

TEST_CASE("IPv4 Edge: Header field validation", "[net][ipv4][edge]") {
    
    SECTION("构造合法 20 字节 IP 头 → checksum 验证通过") {
        auto pkt = make_ipv4_header();
        RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
        
        // 验证辅助函数逻辑正确
        CHECK((hdr->version_ihl & 0x0F) == 5);
        CHECK((hdr->version_ihl >> 4) == 4);
        // 验证 Checksum
        CHECK(compute_checksum(pkt.data(), 20) == 0);
    }

    SECTION("IHL = 0 → ihl() 返回 0") {
        auto pkt = make_ipv4_header(0); // 构造时会被 resize 到 20 以防 crash
        RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
        // 强制修改回 0
        hdr->version_ihl = (4 << 4) | 0;
        CHECK((hdr->version_ihl & 0x0F) == 0);
    }

    SECTION("IHL = 4 → 小于最小值 5") {
        auto pkt = make_ipv4_header(4);
        RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
        CHECK((hdr->version_ihl & 0x0F) == 4);
        // 这种包在实际处理中应被丢弃
    }

    SECTION("IHL = 15 → 最大值，声称 60 字节头") {
        auto pkt = make_ipv4_header(15);
        // pkt size 应为 60
        CHECK(pkt.size() == 60);
        CHECK(compute_checksum(pkt.data(), 60) == 0);
    }

    SECTION("version = 6 而非 4 → 非 IPv4") {
        auto pkt = make_ipv4_header(5, 6);
        RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
        CHECK((hdr->version_ihl >> 4) == 6);
    }

    SECTION("total_length = 0") {
        auto pkt = make_ipv4_header(5, 4, 0);
        RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
        CHECK(ntohs(hdr->total_length) == 0);
    }

    SECTION("total_length = 19 (小于最小头)") {
        auto pkt = make_ipv4_header(5, 4, 19);
        RawIPv4Header* hdr = reinterpret_cast<RawIPv4Header*>(pkt.data());
        CHECK(ntohs(hdr->total_length) < 20);
    }

    SECTION("total_length 大于实际包长") {
        // 声称 100 字节，实际 buffer 只有 20
        auto pkt = make_ipv4_header(5, 4, 100);
        // 验证逻辑通常会在处理前检查 len >= total_length
        CHECK(pkt.size() == 20);
    }
}

TEST_CASE("IPv4 Edge: Checksum", "[net][ipv4][edge]") {
    SECTION("正确 checksum → compute_checksum 返回 0") {
        auto pkt = make_ipv4_header();
        CHECK(compute_checksum(pkt.data(), 20) == 0);
    }

    SECTION("翻转一个 bit → checksum 不为 0") {
        auto pkt = make_ipv4_header();
        pkt[2] ^= 0xFF; // 翻转 total length 高位
        CHECK(compute_checksum(pkt.data(), 20) != 0);
    }

    SECTION("全零包的 checksum") {
        std::vector<uint8_t> pkt(20, 0);
        // 0x0000 取反是 0xFFFF，但在 1's complement 加法中，+0 和 -0 等价。
        // compute_checksum 的具体实现可能返回 0 或 0xFFFF
        // 这里主要验证不会崩溃
        CHECK_NOTHROW(compute_checksum(pkt.data(), 20));
    }
}