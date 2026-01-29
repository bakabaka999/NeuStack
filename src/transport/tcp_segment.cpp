#include "neustack/transport/tcp_segment.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <vector>

using namespace neustack;

std::optional<TCPSegment> TCPParser::parse(const IPv4Packet &pkt) {
    // 最小长度检查
    if (pkt.payload_length < sizeof(TCPHeader)) {
        LOG_WARN(TCP, "packet too short (%zu bytes)", pkt.payload_length);
        return std::nullopt;
    }

    auto *hdr = reinterpret_cast<const TCPHeader *>(pkt.payload);

    // 头部长度检查
    uint8_t tcp_hdr_len = hdr->header_length();
    if (tcp_hdr_len < sizeof(TCPHeader) || tcp_hdr_len > pkt.payload_length) {
        LOG_WARN(TCP, "invalid TCP data offset: %u, pkt_len: %zu",
                 tcp_hdr_len, pkt.payload_length);
        return std::nullopt;
    }

    // 校验和验证
    if (!verify_checksum(pkt)) {
        LOG_WARN(TCP, "checksum error from %s:%u",
                 ip_to_string(pkt.src_addr).c_str(), ntohs(hdr->src_port));
        return std::nullopt;
    }

    // 解析 TCP 段
    TCPSegment seg{};

    // IP 层信息
    seg.src_addr = pkt.src_addr;
    seg.dst_addr = pkt.dst_addr;

    // TCP 头部字段 (网络字节序 -> 主机字节序)
    seg.src_port    = ntohs(hdr->src_port);
    seg.dst_port    = ntohs(hdr->dst_port);
    seg.seq_num     = ntohl(hdr->seq_num);
    seg.ack_num     = ntohl(hdr->ack_num);
    seg.data_offset = tcp_hdr_len;
    seg.flags       = hdr->flags;
    seg.window      = ntohs(hdr->window);
    seg.checksum    = ntohs(hdr->checksum);
    seg.urgent_ptr  = ntohs(hdr->urgent_ptr);

    // 数据部分：头部之后的内容
    seg.data        = pkt.payload + tcp_hdr_len;
    seg.data_length = pkt.payload_length - tcp_hdr_len;

    LOG_DEBUG(TCP, "%s:%u -> %s:%u [%s%s%s%s%s] seq=%u ack=%u win=%u len=%zu",
              ip_to_string(seg.src_addr).c_str(), seg.src_port,
              ip_to_string(seg.dst_addr).c_str(), seg.dst_port,
              seg.is_syn() ? "SYN " : "",
              seg.is_ack() ? "ACK " : "",
              seg.is_fin() ? "FIN " : "",
              seg.is_rst() ? "RST " : "",
              seg.is_psh() ? "PSH " : "",
              seg.seq_num, seg.ack_num, seg.window, seg.data_length);

    return seg;
}

bool TCPParser::verify_checksum(const IPv4Packet &pkt) {
    uint16_t checksum = compute_tcp_checksum(pkt.src_addr, pkt.dst_addr,
                                             pkt.payload, pkt.payload_length);
    return checksum == 0;
}

uint16_t TCPParser::compute_tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                         const uint8_t *tcp_data, size_t tcp_len) {
    // TCP 段长度不能超过 65535（伪头部长度字段为 16 位）
    if (tcp_len > 65535) {
        LOG_ERROR(TCP, "TCP segment too large for checksum: %zu", tcp_len);
        return 0;
    }

    // 构造伪头部 + TCP 数据
    std::vector<uint8_t> buffer(sizeof(TCPPseudoHeader) + tcp_len);

    // 填充伪头部
    auto *pseudo = reinterpret_cast<TCPPseudoHeader *>(buffer.data());
    pseudo->src_addr = htonl(src_ip);
    pseudo->dst_addr = htonl(dst_ip);
    pseudo->zero = 0;
    pseudo->protocol = 6; // TCP
    pseudo->tcp_length = htons(static_cast<uint16_t>(tcp_len));

    // 复制 TCP 数据
    std::memcpy(buffer.data() + sizeof(TCPPseudoHeader), tcp_data, tcp_len);

    return compute_checksum(buffer.data(), buffer.size());
}
