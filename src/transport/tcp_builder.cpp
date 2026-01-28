#include "neustack/transport/tcp_builder.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <vector>

using namespace neustack;

ssize_t TCPBuilder::build(uint8_t *buffer, size_t buffer_len) const {
    constexpr size_t HEADER_LEN = sizeof(TCPHeader);  // 20 字节（无选项）
    size_t total_len = HEADER_LEN + _payload_len;

    // 缓冲区检查
    if (buffer_len < total_len) {
        LOG_ERROR(TCP, "build failed: buffer too small (%zu < %zu)", buffer_len, total_len);
        return -1;
    }

    auto *hdr = reinterpret_cast<TCPHeader *>(buffer);

    // 填充头部（主机字节序 -> 网络字节序）
    hdr->src_port    = htons(_src_port);
    hdr->dst_port    = htons(_dst_port);
    hdr->seq_num     = htonl(_seq);
    hdr->ack_num     = htonl(_ack);
    hdr->data_offset = (HEADER_LEN / 4) << 4;  // 高 4 位是数据偏移，单位 4 字节
    hdr->flags       = _flags;
    hdr->window      = htons(_window);
    hdr->checksum    = 0;  // 先设为 0，后续由 fill_checksum 填充
    hdr->urgent_ptr  = 0;

    // 复制 payload
    if (_payload && _payload_len > 0) {
        std::memcpy(buffer + HEADER_LEN, _payload, _payload_len);
    }

    return static_cast<ssize_t>(total_len);
}

void TCPBuilder::fill_checksum(uint8_t *tcp_data, size_t tcp_len,
                               uint32_t src_ip, uint32_t dst_ip) {
    // 先将校验和字段清零
    auto *hdr = reinterpret_cast<TCPHeader *>(tcp_data);
    hdr->checksum = 0;

    // 构造伪头部 + TCP 数据
    std::vector<uint8_t> buffer(sizeof(TCPPseudoHeader) + tcp_len);

    // 填充伪头部
    auto *pseudo = reinterpret_cast<TCPPseudoHeader *>(buffer.data());
    pseudo->src_addr = htonl(src_ip);
    pseudo->dst_addr = htonl(dst_ip);
    pseudo->zero = 0;
    pseudo->protocol = 6;  // TCP
    pseudo->tcp_length = htons(static_cast<uint16_t>(tcp_len));

    // 复制 TCP 数据
    std::memcpy(buffer.data() + sizeof(TCPPseudoHeader), tcp_data, tcp_len);

    // 计算校验和并填入
    hdr->checksum = compute_checksum(buffer.data(), buffer.size());
}
