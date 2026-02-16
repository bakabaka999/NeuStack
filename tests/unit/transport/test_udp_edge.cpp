#include <catch2/catch_test_macros.hpp>
#include "neustack/transport/udp.hpp"
#include "neustack/common/checksum.hpp"
#include <cstring>
#include <vector>
#include "neustack/common/platform.hpp"

using namespace neustack;

// ============================================================================
// 测试辅助函数
// ============================================================================

/**
 * @brief 构造一个原始 UDP 包（仅 UDP 头部 + 数据）
 */
std::vector<uint8_t> make_raw_udp_packet(uint16_t src_port, uint16_t dst_port, 
                                         uint16_t len_field, uint16_t checksum,
                                         const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> pkt(8 + payload.size());
    UDPHeader* hdr = reinterpret_cast<UDPHeader*>(pkt.data());
    
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons(len_field);
    hdr->checksum = checksum; // 直接设置，不计算

    if (!payload.empty()) {
        std::memcpy(pkt.data() + 8, payload.data(), payload.size());
    }
    return pkt;
}

/**
 * @brief 按照 RFC 768 手动计算 UDP 校验和（包含伪头部）
 */
uint16_t calculate_full_udp_checksum(uint32_t src_ip, uint32_t dst_ip, 
                                     const std::vector<uint8_t>& udp_segment) {
    // 构造伪头部 + UDP段 的连续内存
    std::vector<uint8_t> buffer;
    buffer.reserve(sizeof(UDPPseudoHeader) + udp_segment.size());

    // 1. 填充伪头部
    UDPPseudoHeader ph;
    ph.src_addr = src_ip;
    ph.dst_addr = dst_ip;
    ph.zero     = 0;
    ph.protocol = 17; // UDP
    ph.udp_length = htons(static_cast<uint16_t>(udp_segment.size()));

    // 2. 拼接
    const uint8_t* ph_ptr = reinterpret_cast<const uint8_t*>(&ph);
    buffer.insert(buffer.end(), ph_ptr, ph_ptr + sizeof(UDPPseudoHeader));
    buffer.insert(buffer.end(), udp_segment.begin(), udp_segment.end());

    // 3. 计算 (compute_checksum 内部已包含求和、进位和取反)
    return compute_checksum(buffer.data(), buffer.size());
}

// ============================================================================
// 测试用例
// ============================================================================

TEST_CASE("UDP Edge: Header field validation", "[transport][udp][edge]") {

    SECTION("结构体大小验证") {
        // 确保编译器没有对结构体进行额外的 padding
        CHECK(sizeof(UDPHeader) == 8);
        CHECK(sizeof(UDPPseudoHeader) == 12);
    }

    SECTION("构造合法 8 字节 UDP 头 (无数据)") {
        std::vector<uint8_t> payload;
        // Length = 8 (header only)
        auto pkt = make_raw_udp_packet(1234, 53, 8, 0, payload);
        
        const UDPHeader* hdr = reinterpret_cast<const UDPHeader*>(pkt.data());
        CHECK(hdr->source_port() == 1234);
        CHECK(hdr->dest_port() == 53);
        CHECK(hdr->data_length() == 0);
    }

    SECTION("length 字段 = 7 (小于最小头 8)") {
        // 这种包在解析时应被视为畸形
        std::vector<uint8_t> payload;
        auto pkt = make_raw_udp_packet(1234, 53, 7, 0, payload);
        
        const UDPHeader* hdr = reinterpret_cast<const UDPHeader*>(pkt.data());
        // 验证读取出的值确实是 7，后续逻辑应据此丢弃
        CHECK(ntohs(hdr->length) == 7);
        // 如果使用 data_length() 辅助函数，可能会发生下溢出
        // 8 - 8 = 0, 7 - 8 = 65535 (uint16 wrap)
        CHECK(static_cast<uint16_t>(ntohs(hdr->length) - 8) > 65000); 
    }

    SECTION("length 字段 = 0") {
        std::vector<uint8_t> payload;
        auto pkt = make_raw_udp_packet(1234, 53, 0, 0, payload);
        const UDPHeader* hdr = reinterpret_cast<const UDPHeader*>(pkt.data());
        CHECK(ntohs(hdr->length) == 0);
    }

    SECTION("length 字段 > 实际数据长度") {
        // 声称 100 字节，实际只有 8 字节
        std::vector<uint8_t> payload;
        auto pkt = make_raw_udp_packet(1234, 53, 100, 0, payload);
        
        // 验证数据包本身只有 8 字节
        CHECK(pkt.size() == 8);
        const UDPHeader* hdr = reinterpret_cast<const UDPHeader*>(pkt.data());
        CHECK(ntohs(hdr->length) == 100);
        // 此类包在处理时应因 buffer overrun 检查被丢弃
    }

    SECTION("端口号 0 和 65535 边界") {
        std::vector<uint8_t> payload;
        auto pkt = make_raw_udp_packet(0, 65535, 8, 0, payload);
        const UDPHeader* hdr = reinterpret_cast<const UDPHeader*>(pkt.data());
        
        CHECK(hdr->source_port() == 0);
        CHECK(hdr->dest_port() == 65535);
    }
}

TEST_CASE("UDP Edge: Checksum", "[transport][udp][edge]") {
    uint32_t src = htonl(0x7F000001); // 127.0.0.1
    uint32_t dst = htonl(0x7F000001);

    SECTION("checksum = 0 (UDP 允许关闭校验)") {
        std::vector<uint8_t> payload = {'A', 'B', 'C'};
        // Length = 8 + 3 = 11
        auto pkt = make_raw_udp_packet(100, 200, 11, 0, payload);
        
        const UDPHeader* hdr = reinterpret_cast<const UDPHeader*>(pkt.data());
        CHECK(hdr->checksum == 0);
        // 在 UDP 中，如果接收到的 Checksum 字段为 0，表示发送端未计算校验和，接收端应跳过验证
    }

    SECTION("正确 checksum 验证") {
        std::vector<uint8_t> payload = {0x01, 0x02};
        // 1. 先构造 Checksum 为 0 的包
        auto pkt = make_raw_udp_packet(100, 200, 10, 0, payload);
        
        // 2. 计算正确 Checksum
        uint16_t csum = calculate_full_udp_checksum(src, dst, pkt);
        
        // RFC 768: 如果计算结果是 0，则填入 0xFFFF (因为 0 代表不校验)
        // 但 compute_checksum 通常返回 ~sum。
        // 如果是 0x0000，则填 0xFFFF；如果是 0xFFFF，填 0xFFFF？
        // 这里的 compute_checksum 是通用实现。
        // 我们只验证非 0 情况。
        
        if (csum == 0) csum = 0xFFFF;
        
        // 3. 填入 Checksum
        UDPHeader* hdr = reinterpret_cast<UDPHeader*>(pkt.data());
        hdr->checksum = csum;

        // 4. 验证：重新计算整个（伪头+UDP）的校验和，结果应为 0 (或 0xFFFF)
        // 注意：校验时，checksum 字段已在包内。
        // compute_checksum(buffer) 应该返回 0。
        uint16_t verify_res = calculate_full_udp_checksum(src, dst, pkt);
        CHECK(verify_res == 0);
    }

    SECTION("错误 checksum 验证") {
        std::vector<uint8_t> payload = {0xFF};
        auto pkt = make_raw_udp_packet(100, 200, 9, 0, payload);
        
        // 填入错误的 Checksum
        UDPHeader* hdr = reinterpret_cast<UDPHeader*>(pkt.data());
        hdr->checksum = 0xDEAD; 

        // 验证：结果不应为 0
        uint16_t verify_res = calculate_full_udp_checksum(src, dst, pkt);
        CHECK(verify_res != 0);
    }
}