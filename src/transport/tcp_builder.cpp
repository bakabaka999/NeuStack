#include "neustack/transport/tcp_builder.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include "neustack/common/platform.hpp"

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

ssize_t TCPBuilder::build_header_only(uint8_t *buffer, size_t buffer_len) const {
    constexpr size_t HEADER_LEN = sizeof(TCPHeader);

    if (buffer_len < HEADER_LEN) {
        LOG_ERROR(TCP, "build_header_only: buffer too small (%zu < %zu)", buffer_len, HEADER_LEN);
        return -1;
    }

    auto *hdr = reinterpret_cast<TCPHeader *>(buffer);

    hdr->src_port    = htons(_src_port);
    hdr->dst_port    = htons(_dst_port);
    hdr->seq_num     = htonl(_seq);
    hdr->ack_num     = htonl(_ack);
    hdr->data_offset = (HEADER_LEN / 4) << 4;
    hdr->flags       = _flags;
    hdr->window      = htons(_window);
    hdr->checksum    = 0;
    hdr->urgent_ptr  = 0;

    return static_cast<ssize_t>(HEADER_LEN);
}

void TCPBuilder::fill_checksum(uint8_t *tcp_data, size_t tcp_len,
                               uint32_t src_ip, uint32_t dst_ip) {
    if (tcp_len > 65535) {
        LOG_ERROR(TCP, "TCP segment too large for checksum: %zu", tcp_len);
        return;
    }

    auto *hdr = reinterpret_cast<TCPHeader *>(tcp_data);
    hdr->checksum = 0;

    // 栈上伪头部 + 分段累加，零拷贝
    TCPPseudoHeader pseudo;
    pseudo.src_addr = htonl(src_ip);
    pseudo.dst_addr = htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.protocol = 6;
    pseudo.tcp_length = htons(static_cast<uint16_t>(tcp_len));

    uint32_t sum = 0;
    sum = checksum_accumulate(sum, &pseudo, sizeof(pseudo));
    sum = checksum_accumulate(sum, tcp_data, tcp_len);
    hdr->checksum = checksum_finalize(sum);
}
