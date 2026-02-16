/**
 * @file ipv4.cpp
 * @brief IPv4 layer implementation
 */

#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include "neustack/common/platform.hpp"
#include <cstring>

using namespace neustack;

// ============================================================================
// IPv4Parser
// ============================================================================

std::optional<IPv4Packet> IPv4Parser::parse(const uint8_t* data, size_t len) {
    // 首先验证有没有错误
    const char* err = validate(data, len);
    if (err != nullptr) {
        LOG_TRACE(IPv4, "parse failed: %s", err);
        return std::nullopt;
    }

    // 直接把数据转换为网络字节序的 IPv4 头部
    auto* hdr = reinterpret_cast<const IPv4Header*>(data);
    IPv4Packet pkt{};

    // 解析头部字段
    pkt.version = hdr->version();
    pkt.ihl = hdr->ihl();
    pkt.dscp = hdr->dscp_ecn >> 2;
    pkt.ecn = hdr->dscp_ecn & 0x03;
    pkt.total_length = ntohs(hdr->total_length);
    pkt.identification = ntohs(hdr->identification);
    pkt.dont_fragment = hdr->dont_fragment();
    pkt.more_fragments = hdr->more_fragments();
    pkt.fragment_offset = hdr->fragment_offset() * 8;  // 转换为字节
    pkt.ttl = hdr->ttl;
    pkt.protocol = hdr->protocol;
    pkt.checksum = ntohs(hdr->checksum);
    pkt.src_addr = ntohl(hdr->src_addr);
    pkt.dst_addr = ntohl(hdr->dst_addr);

    // Payload
    size_t header_len = hdr->header_length();
    pkt.payload = data + header_len;
    pkt.payload_length = pkt.total_length - header_len;

    // 原始数据
    pkt.raw_data = data;
    pkt.raw_length = len;

    return pkt;
}

const char* IPv4Parser::validate(const uint8_t* data, size_t len) {
    // 最小长度检查
    if (len < 20) {
        return "packet too short";
    }

    auto* hdr = reinterpret_cast<const IPv4Header*>(data);

    // 版本检查
    if (hdr->version() != 4) {
        return "not IPv4";
    }

    // IHL 检查
    if (hdr->ihl() < 5) {
        return "invalid IHL";
    }

    // 头部长度不能超过数据长度
    size_t header_len = hdr->header_length();
    if (header_len > len) {
        return "header length exceeds packet";
    }

    // Total Length 检查
    uint16_t total_len = ntohs(hdr->total_length);
    if (total_len < header_len) {
        return "total length < header length";
    }
    if (total_len > len) {
        return "total length exceeds packet";
    }

    // 校验和验证
    if (!verify_checksum(data, header_len)) {
        return "checksum error";
    }

    return nullptr;  // 无错误
}

// ============================================================================
// IPv4Builder
// ============================================================================

ssize_t IPv4Builder::build(uint8_t* buffer, size_t buffer_len) const {
    constexpr size_t HEADER_LEN = 20;
    size_t total_len = HEADER_LEN + _payload_len;

    if (buffer_len < total_len) {
        LOG_ERROR(IPv4, "build failed: buffer too small (%zu < %zu)", buffer_len, total_len);
        return -1;
    }

    if (total_len > 65535) {
        LOG_ERROR(IPv4, "build failed: packet too large (%zu)", total_len);
        return -1;
    }

    auto* hdr = reinterpret_cast<IPv4Header*>(buffer);

    // 填充头部
    hdr->version_ihl = (4 << 4) | 5;  // IPv4, IHL=5
    hdr->dscp_ecn = (_dscp << 2) | (_ecn & 0x03);
    hdr->total_length = htons(static_cast<uint16_t>(total_len));
    hdr->identification = htons(_identification);

    uint16_t flags_frag = 0;
    if (_dont_fragment) {
        flags_frag |= (0x02 << 13);  // DF flag
    }
    hdr->flags_fragment = htons(flags_frag);

    hdr->ttl = _ttl;
    hdr->protocol = _protocol;
    hdr->checksum = 0;  // 先设为 0
    hdr->src_addr = htonl(_src_addr);
    hdr->dst_addr = htonl(_dst_addr);

    // 计算校验和
    hdr->checksum = compute_checksum(hdr, HEADER_LEN);

    // 复制 payload
    if (_payload && _payload_len > 0) {
        std::memcpy(buffer + HEADER_LEN, _payload, _payload_len);
    }

    return static_cast<ssize_t>(total_len);
}

// ============================================================================
// IPv4Layer
// ============================================================================

namespace {

// 判断是否应该发送 ICMP 错误 (RFC 1122)
bool should_send_icmp_error(const IPv4Packet& pkt) {
    // 1. 源地址为 0.0.0.0
    if (pkt.src_addr == 0) {
        return false;
    }

    // 2. 源地址为广播地址
    if (pkt.src_addr == 0xFFFFFFFF) {
        return false;
    }

    // 3. 源地址为多播地址 (224.0.0.0 - 239.255.255.255)
    uint8_t first_octet = (pkt.src_addr >> 24) & 0xFF;
    if (first_octet >= 224 && first_octet <= 239) {
        return false;
    }

    // 4. 目标地址为广播地址
    if (pkt.dst_addr == 0xFFFFFFFF) {
        return false;
    }

    // 5. 目标地址为多播地址
    first_octet = (pkt.dst_addr >> 24) & 0xFF;
    if (first_octet >= 224 && first_octet <= 239) {
        return false;
    }

    // 6. 分片包的非首片 (fragment_offset > 0)
    if (pkt.fragment_offset > 0) {
        return false;
    }

    return true;
}

// 获取 ICMP handler (辅助函数)
ICMPHandler* get_icmp_handler(const std::unordered_map<uint8_t, IProtocolHandler*>& handlers) {
    auto it = handlers.find(static_cast<uint8_t>(IPProtocol::ICMP));
    if (it != handlers.end()) {
        return dynamic_cast<ICMPHandler*>(it->second);
    }
    return nullptr;
}

}  // namespace

IPv4Layer::IPv4Layer(NetDevice& device)
    : _device(device)
    , _local_ip(0)
    , _mtu(1500)
    , _next_id(0)
{}

void IPv4Layer::register_handler(uint8_t protocol, IProtocolHandler* handler) {
    if (handler) {
        _handlers[protocol] = handler;
        LOG_DEBUG(IPv4, "registered handler for protocol %u", protocol);
    }
}

IProtocolHandler* IPv4Layer::get_handler(uint8_t protocol) const {
    auto it = _handlers.find(protocol);
    return it != _handlers.end() ? it->second : nullptr;
}

void IPv4Layer::on_receive(const uint8_t* data, size_t len) {
    auto pkt = IPv4Parser::parse(data, len);
    if (!pkt) {
        // 解析失败，丢弃
        return;
    }

    // 检查目标地址: 是否是发给我们的或广播
    if (pkt->dst_addr != _local_ip && pkt->dst_addr != 0xFFFFFFFF) {
        // 不是发给我们的，丢弃 (转发暂不实现)
        LOG_TRACE(IPv4, "not for us: dst=%s", ip_to_string(pkt->dst_addr).c_str());
        return;
    }

    // 检查 TTL
    if (pkt->ttl == 0) {
        LOG_WARN(IPv4, "TTL=0 from %s", ip_to_string(pkt->src_addr).c_str());
        if (should_send_icmp_error(*pkt)) {
            if (auto* icmp = get_icmp_handler(_handlers)) {
                icmp->send_time_exceeded(ICMPTimeExCode::TTLExceeded, *pkt);
            }
        }
        return;
    }

    // 分发到上层协议处理器
    auto it = _handlers.find(pkt->protocol);
    if (it != _handlers.end()) {
        it->second->handle(*pkt);
    } else {
        LOG_WARN(IPv4, "unknown protocol %u from %s", pkt->protocol, ip_to_string(pkt->src_addr).c_str());
        if (should_send_icmp_error(*pkt)) {
            if (auto* icmp = get_icmp_handler(_handlers)) {
                icmp->send_dest_unreachable(ICMPUnreachCode::ProtocolUnreachable, *pkt);
            }
        }
    }
}

ssize_t IPv4Layer::send(uint32_t dst, uint8_t protocol,
                        const uint8_t* payload, size_t len) {
    constexpr size_t IP_HEADER_LEN = 20;

    // 检查是否超过 MTU (暂不实现分片，设置 DF=1)
    if (IP_HEADER_LEN + len > _mtu) {
        LOG_ERROR(IPv4, "packet too large (%zu > MTU %u), fragmentation not supported",
                  IP_HEADER_LEN + len, _mtu);
        return -1;
    }

    uint8_t buffer[1500];

    // 使用 IPv4Builder 构建报文
    ssize_t n = IPv4Builder()
        .set_src(_local_ip)
        .set_dst(dst)
        .set_protocol(protocol)
        .set_identification(_next_id++)
        .set_dont_fragment(true)  // 暂不支持分片
        .set_ttl(64)
        .set_payload(payload, len)
        .build(buffer, sizeof(buffer));

    if (n < 0) {
        return -1;
    }

    LOG_TRACE(IPv4, "sending %zd bytes to %s, proto=%u", n, ip_to_string(dst).c_str(), protocol);
    return _device.send(buffer, n);
}
