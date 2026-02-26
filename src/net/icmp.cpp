#include "neustack/net/icmp.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <algorithm>
#include <cstring>

using namespace neustack;

void ICMPHandler::handle(const IPv4Packet &pkt) {
    // 最小长度检查
    if (pkt.payload_length < sizeof(ICMPHeader)) {
        LOG_WARN(ICMP, "packet too short (%zu bytes)", pkt.payload_length);
        return;
    }

    // 验证校验和
    if (!verify_checksum(pkt.payload, pkt.payload_length)) {
        LOG_WARN(ICMP, "checksum error from %s", ip_to_string(pkt.src_addr).c_str());
        return;
    }

    auto *hdr = reinterpret_cast<const ICMPHeader *>(pkt.payload);

    // 根据类型分发
    switch (static_cast<ICMPType>(hdr->type)) {
        case ICMPType::EchoRequest:
            handle_echo_request(pkt);
            break;

        case ICMPType::EchoReply:
            handle_echo_reply(pkt);
            break;

        case ICMPType::DestUnreachable:
            LOG_INFO(ICMP, "Dest Unreachable from %s, code=%u",
                     ip_to_string(pkt.src_addr).c_str(), hdr->code);
            // TODO: 解析原始包，通知对应的 socket
            break;

        case ICMPType::TimeExceeded:
            LOG_INFO(ICMP, "Time Exceeded from %s, code=%u",
                     ip_to_string(pkt.src_addr).c_str(), hdr->code);
            // TODO: 解析原始包，通知对应的 socket
            break;

        default:
            LOG_DEBUG(ICMP, "unhandled type %u from %s", hdr->type,
                      ip_to_string(pkt.src_addr).c_str());
            break;
    }
}

void ICMPHandler::handle_echo_request(const IPv4Packet &pkt) {
    size_t icmp_len = pkt.payload_length;

    if (icmp_len < sizeof(ICMPHeader) + sizeof(ICMPEcho)) {
        LOG_WARN(ICMP, "echo request too short");
        return;
    }

    auto *hdr = reinterpret_cast<const ICMPHeader *>(pkt.payload);
    auto *echo = reinterpret_cast<const ICMPEcho *>(hdr + 1);

    // Echo Data 部分
    const uint8_t *data = reinterpret_cast<const uint8_t *>(echo + 1);
    size_t data_len = icmp_len - sizeof(ICMPHeader) - sizeof(ICMPEcho);

    LOG_INFO(ICMP, "Echo Request from %s, id=%u, seq=%u, data_len=%zu",
             ip_to_string(pkt.src_addr).c_str(),
             ntohs(echo->identifier),
             ntohs(echo->sequence),
             data_len);

    // 构造 Echo Reply
    size_t reply_len = pkt.payload_length; // 和请求一样长
    uint8_t reply_buf[1500];

    if (reply_len > sizeof(reply_buf)) {
        LOG_ERROR(ICMP, "echo reply too large");
        return;
    }

    // Echo Reply三个位置的指针
    auto *reply_hdr = reinterpret_cast<ICMPHeader *>(reply_buf);
    auto *reply_echo = reinterpret_cast<ICMPEcho *>(reply_hdr + 1);
    uint8_t *reply_data = reinterpret_cast<uint8_t *>(reply_echo + 1);

    // 首先构建 ICMP HEADER
    reply_hdr->type = static_cast<uint8_t>(ICMPType::EchoReply);
    reply_hdr->code = 0;
    reply_hdr->checksum = 0;

    // 然后构建 ICMP ECHO，原样返回
    reply_echo->identifier = echo->identifier;
    reply_echo->sequence = echo->sequence;

    // Data同样原样返回
    if (data_len > 0) {
        std::memcpy(reply_data, data, data_len);
    }

    // 计算校验和
    reply_hdr->checksum = compute_checksum(reply_buf, reply_len);

    // 发送
    _ip_layer.send(pkt.src_addr,
                   static_cast<uint8_t>(IPProtocol::ICMP),
                   reply_buf, reply_len);

    LOG_DEBUG(ICMP, "Echo Reply sent to %s", ip_to_string(pkt.src_addr).c_str());
}

void ICMPHandler::handle_echo_reply(const IPv4Packet &pkt) {
    if (pkt.payload_length < sizeof(ICMPHeader) + sizeof(ICMPEcho)) {
        LOG_WARN(ICMP, "echo reply too short");
        return;
    }

    auto *hdr = reinterpret_cast<const ICMPHeader *>(pkt.payload);
    auto *echo = reinterpret_cast<const ICMPEcho *>(hdr + 1);
    (void)hdr;

    uint16_t id  = ntohs(echo->identifier);
    uint16_t seq = ntohs(echo->sequence);

    LOG_DEBUG(ICMP, "Echo Reply from %s, id=%u, seq=%u",
             ip_to_string(pkt.src_addr).c_str(), id, seq);

    if (_reply_cb) {
        _reply_cb(pkt.src_addr, id, seq, 0);
    }
}

void ICMPHandler::send_dest_unreachable(ICMPUnreachCode code, const IPv4Packet &original_pkt) {
    // 检查原本的包的性质，不对ICMP错误回复ICMP错误
    if (original_pkt.protocol == static_cast<uint8_t>(IPProtocol::ICMP)) {
        // 确保有足够的数据来读取 ICMP 头部
        if (original_pkt.payload_length < sizeof(ICMPHeader)) {
            return;
        }
        auto *icmp_hdr = reinterpret_cast<const ICMPHeader *>(original_pkt.payload);
        if (icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoRequest) &&
            icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoReply)) {
            LOG_TRACE(ICMP, "not sending error for ICMP error message");
            return;
        }
    }

    uint32_t extra = 0;

    send_error(static_cast<uint8_t>(ICMPType::DestUnreachable),
               static_cast<uint8_t>(code),
               extra,
               original_pkt);

    LOG_DEBUG(ICMP, "Dest Unreachable (code=%u) sent to %s",
              static_cast<uint8_t>(code),
              ip_to_string(original_pkt.src_addr).c_str());
}

void ICMPHandler::send_time_exceeded(ICMPTimeExCode code, const IPv4Packet &original_pkt) {
    // 检查是否为 ICMP 错误回复
    if (original_pkt.protocol == static_cast<uint8_t>(IPProtocol::ICMP)) {
        // 确保有足够的数据来读取 ICMP 头部
        if (original_pkt.payload_length < sizeof(ICMPHeader)) {
            return;
        }
        auto *icmp_hdr = reinterpret_cast<const ICMPHeader *>(original_pkt.payload);
        if (icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoRequest) &&
            icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoReply)) {
            LOG_TRACE(ICMP, "not sending error for ICMP error message");
            return;
        }
    }

    send_error(static_cast<uint8_t>(ICMPType::TimeExceeded),
               static_cast<uint8_t>(code),
               0,
               original_pkt);

    LOG_DEBUG(ICMP, "Time Exceeded (code=%u) sent to %s",
              static_cast<uint8_t>(code),
              ip_to_string(original_pkt.src_addr).c_str());
}

void ICMPHandler::send_error(uint8_t type, uint8_t code, uint32_t extra, const IPv4Packet &original_pkt) {
    // 格式：ICMP Header (4) + Extra (4) + Original IP Header + 8 bytes

    // 复制原始 IP 头部 + 最多8字节payload
    size_t orig_ip_hdr_len = original_pkt.ihl * 4;
    size_t orig_data_len = std::min(original_pkt.payload_length, size_t(8));
    size_t orig_copy_len = orig_ip_hdr_len + orig_data_len;

    size_t icmp_len = sizeof(ICMPHeader) + sizeof(ICMPError) + orig_copy_len;
    uint8_t icmp_buf[256];

    if (icmp_len > sizeof(icmp_buf)) {
        LOG_ERROR(ICMP, "error message too large");
        return;
    }

    auto *hdr = reinterpret_cast<ICMPHeader *>(icmp_buf);
    auto *err = reinterpret_cast<ICMPError *>(hdr + 1);
    uint8_t *orig = reinterpret_cast<uint8_t *>(err + 1);

    // ICMP头部
    hdr->type = type;
    hdr->code = code;
    hdr->checksum = 0;

    // Extra字段
    err->unused = (extra >> 16) & 0xffff;
    err->next_hop_mtu = extra & 0xffff;

    // 复制原始 IP 头部
    std::memcpy(orig, original_pkt.raw_data, orig_ip_hdr_len);

    // 复制原始 payload 的前 8 字节
    if (orig_data_len > 0) {
        std::memcpy(orig + orig_ip_hdr_len, original_pkt.payload, orig_data_len);
    }

    // 计算校验和
    hdr->checksum = compute_checksum(icmp_buf, icmp_len);

    // 发给原始包的发送者
    _ip_layer.send(original_pkt.src_addr,
                   static_cast<uint8_t>(IPProtocol::ICMP),
                   icmp_buf, icmp_len);
}

void ICMPHandler::send_echo_request(uint32_t dst_ip, uint16_t identifier, uint16_t sequence,
                                     const uint8_t *data, size_t data_len) {
    size_t icmp_len = sizeof(ICMPHeader) + sizeof(ICMPEcho) + data_len;
    uint8_t icmp_buf[1500];

    if (icmp_len > sizeof(icmp_buf)) {
        LOG_ERROR(ICMP, "echo request too large");
        return;
    }

    auto *hdr = reinterpret_cast<ICMPHeader *>(icmp_buf);
    auto *echo = reinterpret_cast<ICMPEcho *>(hdr + 1);
    uint8_t *payload = reinterpret_cast<uint8_t *>(echo + 1);

    // ICMP 头部
    hdr->type = static_cast<uint8_t>(ICMPType::EchoRequest);
    hdr->code = 0;
    hdr->checksum = 0;

    // Echo 头部
    echo->identifier = htons(identifier);
    echo->sequence = htons(sequence);

    // 复制数据
    if (data && data_len > 0) {
        std::memcpy(payload, data, data_len);
    }

    // 计算校验和
    hdr->checksum = compute_checksum(icmp_buf, icmp_len);

    // 发送
    _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::ICMP), icmp_buf, icmp_len);

    LOG_DEBUG(ICMP, "Echo Request sent to %s, id=%u, seq=%u",
              ip_to_string(dst_ip).c_str(), identifier, sequence);
}
