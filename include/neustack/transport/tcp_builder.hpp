#ifndef NEUSTACK_TRANSPORT_TCP_BUILDER_HPP
#define NEUSTACK_TRANSPORT_TCP_BUILDER_HPP

#include <cstdint>
#include <cstring>
#include <vector>

#include "neustack/transport/tcp.hpp"

namespace neustack {

// ========================================================================
// TCP 段构建器
// ========================================================================

class TCPBuilder {
public:
    TCPBuilder &set_src_port(uint16_t port) { _src_port = port; return *this; }
    TCPBuilder& set_dst_port(uint16_t port) { _dst_port = port; return *this; }
    TCPBuilder& set_seq(uint32_t seq) { _seq = seq; return *this; }
    TCPBuilder& set_ack(uint32_t ack) { _ack = ack; return *this; }
    TCPBuilder& set_flags(uint8_t flags) { _flags = flags; return *this; }
    TCPBuilder& set_window(uint16_t window) { _window = window; return *this; }
    TCPBuilder& set_payload(const uint8_t* data, size_t len) {
        _payload = data;
        _payload_len = len;
        return *this;
    }

    // 构建 TCP 段（不包含校验和）
    // 返回构建的长度，-1表示失败
    ssize_t build(uint8_t *buffer, size_t buffer_len) const;

    // 计算并填充校验和
    static void fill_checksum(uint8_t *tcp_data, size_t tcp_len,
                              uint32_t src_ip, uint32_t dst_ip);

private:
    uint16_t _src_port = 0;             // 源端口
    uint16_t _dst_port = 0;             // 目的端口
    uint32_t _seq = 0;                  // 报文SEQ号
    uint32_t _ack = 0;                  // 报文ACK号
    uint8_t _flags = 0;                 // 报文FLAGS位
    uint16_t _window = 65535;           // 窗口大小
    const uint8_t *_payload = nullptr;  // 负载数据位置
    size_t _payload_len = 0;            // 负载数据长度
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_BUILDER_HPP
