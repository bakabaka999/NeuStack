#include "neustack/transport/udp.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <cstring>
#include <vector>

using namespace neustack;

UDPLayer::UDPLayer(IPv4Layer &ip_layer)
    : _ip_layer(ip_layer), _next_ephemeral_port(49152)
{}

void UDPLayer::handle(const IPv4Packet &pkt) {
    auto datagram = parse(pkt);

    if (!datagram) {
        return;
    }

    LOG_DEBUG(UDP, "%s:%u -> %s:%u, len=%zu",
              ip_to_string(datagram->src_addr).c_str(),
              datagram->src_port,
              ip_to_string(datagram->dst_addr).c_str(),
              datagram->dst_port,
              datagram->data_length);

    // 查找绑定的 socket
    auto it = _sockets.find(datagram->dst_port);
    if (it != _sockets.end()) {
        // 调用回调函数
        it->second.on_receive(datagram->src_addr, datagram->src_port,
                              datagram->data, datagram->data_length);
    } else {
        // 端口未绑定，发送 ICMP Port Unreachable
        LOG_DEBUG(UDP, "port %u not bound, sending ICMP Port Unreachable", datagram->dst_port);
        auto icmp = dynamic_cast<ICMPHandler *>(_ip_layer.get_handler(static_cast<uint8_t>(IPProtocol::ICMP)));
        if (icmp != nullptr) {
            icmp->send_dest_unreachable(ICMPUnreachCode::PortUnreachable, pkt);
        }
    }
}

std::optional<UDPDatagram> UDPLayer::parse(const IPv4Packet &pkt) {
    // 最小长度检查
    if (pkt.payload_length < sizeof(UDPHeader)) {
        LOG_WARN(UDP, "packet too short (%zu bytes)", pkt.payload_length);
        return std::nullopt;
    }

    auto *hdr = reinterpret_cast<const UDPHeader *>(pkt.payload);

    // 长度检查
    uint16_t udp_len = ntohs(hdr->length);
    if (udp_len < 8 || udp_len > pkt.payload_length) {
        LOG_WARN(UDP, "invalid length field: %u", udp_len);
        return std::nullopt;
    }

    // 校验和验证（如果不为0）
    if (hdr->checksum != 0 && !verify_checksum(pkt)) {
        LOG_WARN(UDP, "checksum error from %s:%u",
                 ip_to_string(pkt.src_addr).c_str(), ntohs(hdr->src_port));
        return std::nullopt;
    }

    // 解析 UDP 数据报
    UDPDatagram datagram{};
    datagram.src_addr = pkt.src_addr;
    datagram.dst_addr = pkt.dst_addr;
    datagram.src_port = ntohs(hdr->src_port);
    datagram.dst_port = ntohs(hdr->dst_port);
    datagram.length = udp_len;
    datagram.checksum = ntohs(hdr->checksum);
    datagram.data = pkt.payload + sizeof(UDPHeader);
    datagram.data_length = udp_len - sizeof(UDPHeader);

    return datagram;
}

bool UDPLayer::verify_checksum(const IPv4Packet &pkt) {
    uint16_t checksum = compute_udp_checksum(pkt.src_addr, pkt.dst_addr,
                                             pkt.payload, pkt.payload_length);
    return checksum == 0;
}

uint16_t UDPLayer::compute_udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                        const uint8_t *udp_packet, size_t udp_len) {
    UDPPseudoHeader pseudo;
    pseudo.src_addr = htonl(src_ip);
    pseudo.dst_addr = htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.protocol = 17;
    pseudo.udp_length = htons(static_cast<uint16_t>(udp_len));

    uint32_t sum = 0;
    sum = checksum_accumulate(sum, &pseudo, sizeof(pseudo));
    sum = checksum_accumulate(sum, udp_packet, udp_len);
    return checksum_finalize(sum);
}

ssize_t UDPLayer::sendto(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                         const uint8_t *data, size_t len) {
    // 检查源端口是否已绑定了
    if (_sockets.find(src_port) == _sockets.end()) {
        LOG_ERROR(UDP, "source port %u not bound", src_port);
        return -1;
    }

    // 构造 UDP 报文
    size_t udp_len = sizeof(UDPHeader) + len;
    std::vector<uint8_t> buffer(udp_len);

    auto *hdr = reinterpret_cast<UDPHeader *>(buffer.data());
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(static_cast<uint16_t>(udp_len));
    hdr->checksum = 0;

    // 复制数据
    if (data && len > 0) {
        std::memcpy(buffer.data() + sizeof(UDPHeader), data, len);
    }

    // 计算校验和
    hdr->checksum = compute_udp_checksum(_ip_layer.local_ip(), dst_ip,
                                         buffer.data(), udp_len);
    if (hdr->checksum == 0) {
        hdr->checksum = 0xffff; // 如果算出来是0我们要设置为0xffff，与不使用校验和区分
    }

    LOG_TRACE(UDP, "sending %zu bytes to %s:%u from port %u",
              len, ip_to_string(dst_ip).c_str(), dst_port, src_port);

    // 通过 IP 层发送
    return _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::UDP),
                          buffer.data(), udp_len);
}

uint16_t UDPLayer::bind(uint16_t port, UDPReceiveCallback callback) {
    if (!callback) {
        LOG_ERROR(UDP, "bind failed: callback is null");
        return 0;
    }

    // 如果端口为0，自动分配
    if (port == 0) {
        port = allocate_ephemeral_port();
        if (port == 0) {
            LOG_ERROR(UDP, "bind failed: no ephemeral ports available");
            return 0;
        }
    }

    // 检查端口是否已被占用
    if (_sockets.find(port) != _sockets.end()) {
        LOG_ERROR(UDP, "bind failed: port %u already bound", port);
        return 0;
    }

    _sockets[port] = BoundSocket{
        .on_receive = std::move(callback),
        .on_error = nullptr
    };
    LOG_INFO(UDP, "bound to port %u", port);
    return port;
}

void UDPLayer::unbind(uint16_t port) {
    auto it = _sockets.find(port);
    if (it != _sockets.end()) {
        _sockets.erase(it);
        LOG_INFO(UDP, "unbound port %u", port);
    }
}

void UDPLayer::set_error_callback(uint16_t port, UDPErrorCallback callback) {
    auto it = _sockets.find(port);
    if (it != _sockets.end()) {
        it->second.on_error = std::move(callback);
    }
}

void UDPLayer::handle_icmp_error(const ICMPErrorInfo &error) {
    auto it = _sockets.find(error.local_port);
    if (it == _sockets.end()) {
        return;
    }

    if (it->second.on_error) {
        it->second.on_error(error);
    }
}

uint16_t UDPLayer::allocate_ephemeral_port() {
    // 临时端口范围: 49152-65535
    constexpr uint16_t EPHEMERAL_PORT_MIN = 49152;
    constexpr uint16_t EPHEMERAL_PORT_MAX = 65535;

    uint16_t start = _next_ephemeral_port;

    do {
        if (_sockets.find(_next_ephemeral_port) == _sockets.end()) {
            uint16_t port = _next_ephemeral_port;
            _next_ephemeral_port++;
            if (_next_ephemeral_port > EPHEMERAL_PORT_MAX) {
                _next_ephemeral_port = EPHEMERAL_PORT_MIN;
            }
            return port;
        }

        _next_ephemeral_port++;
        if (_next_ephemeral_port > EPHEMERAL_PORT_MAX) {
            _next_ephemeral_port = EPHEMERAL_PORT_MIN;
        }
    } while (_next_ephemeral_port != start);

    return 0;  // 所有端口都被占用
}
