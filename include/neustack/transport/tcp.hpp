#ifndef NEUSTACK_TRANSPORT_TCP_HPP
#define NEUSTACK_TRANSPORT_TCP_HPP

#include <cstdint>
#include "neustack/common/platform.hpp"

namespace neustack {

// ========================================================================
// TCP 标志位
// ========================================================================

namespace TCPFlags {
    constexpr uint8_t FIN = 0x01;
    constexpr uint8_t SYN = 0x02;
    constexpr uint8_t RST = 0x04;
    constexpr uint8_t PSH = 0x08;
    constexpr uint8_t ACK = 0x10;
    constexpr uint8_t URG = 0x20;
    constexpr uint8_t ECE = 0x40;
    constexpr uint8_t CWR = 0x80;
}

// ========================================================================
// TCP 头部 (网络字节序)
// ========================================================================

struct  TCPHeader {
    uint16_t src_port;      // 源端口
    uint16_t dst_port;      // 目的端口
    uint32_t seq_num;       // 序列号
    uint32_t ack_num;       // 确认号
    uint8_t  data_offset;   // 数据偏移（高4位）+保留（低4位）
    uint8_t  flags;         // 标识位
    uint16_t window;        // 窗口大小
    uint16_t checksum;      // 校验和
    uint16_t urgent_ptr;    // 紧急指针

    // 辅助方法
    uint8_t header_length() const {
        return (data_offset >> 4) * 4; // 注意，数据偏移单位为字
    }

    bool has_flag(uint8_t flag) const {
        return (flags & flag) != 0;
    }
};

static_assert(sizeof(TCPHeader) == 20, "TCPHeader must be 20 bytes");

// ========================================================================
// TCP 伪头部 (用于校验和)
// ========================================================================

struct TCPPseudoHeader {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint8_t zero;
    uint8_t protocol; // 6 = TCP
    uint16_t tcp_length;
};

static_assert(sizeof(TCPPseudoHeader) == 12, "TCPPseudoHeader must be 12 bytes");

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_HPP
