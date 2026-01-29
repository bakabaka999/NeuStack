#ifndef NEUSTACK_TRANSPORT_TCP_SEGMENT_HPP
#define NEUSTACK_TRANSPORT_TCP_SEGMENT_HPP

#include "neustack/transport/tcp.hpp"
#include "neustack/net/ipv4.hpp"
#include <optional>

namespace neustack {

// ========================================================================
// 解析后的 TCP 段
// ========================================================================

struct TCPSegment {
    // 来源信息（从IP层）
    uint32_t src_addr;
    uint32_t dst_addr;

    // TCP 头部字段（主机字节序）
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;

    // 数据
    const uint8_t *data;
    size_t data_length;

    // 标志判断
    bool is_syn() const { return flags & TCPFlags::SYN; }
    bool is_ack() const { return flags & TCPFlags::ACK; }
    bool is_fin() const { return flags & TCPFlags::FIN; }
    bool is_rst() const { return flags & TCPFlags::RST; }
    bool is_psh() const { return flags & TCPFlags::PSH; }

    // 序列号计算
    // SYN 和 FIN 各自占一个序列号
    uint32_t seg_len() const {
        uint32_t len = data_length;
        if (is_syn()) len++;
        if (is_fin()) len++;
        return len;
    }

    // 段段结束序列号（不含）
    uint32_t seq_end() const {
        return seq_num + seg_len();
    }
};

// ========================================================================
// TCP 解析器
// ========================================================================
class TCPParser {
public:
    // 从 IPv4 包解析TCP段
    static std::optional<TCPSegment> parse(const IPv4Packet &pkt);

    // 验证校验和
    static bool verify_checksum(const IPv4Packet &pkt);

private:
    static uint16_t compute_tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                     const uint8_t *tcp_data, size_t tcp_len);
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_SEGMENT_HPP
