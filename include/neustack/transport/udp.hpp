#ifndef NEUSTACK_TRANSPORT_UDP_HPP
#define NEUSTACK_TRANSPORT_UDP_HPP

#include <arpa/inet.h>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <optional>

#include "neustack/net/ipv4.hpp"
#include "neustack/net/protocol_handler.hpp"

// ============================================================================
// UDP 头部 (网络字节序)
// ============================================================================

struct UDPHeader {
    uint16_t src_port;    // 源端口
    uint16_t dst_port;    // 目标端口
    uint16_t length;      // UDP 长度 (头部 + 数据)
    uint16_t checksum;    // 校验和

    // 辅助方法
    uint16_t source_port() const { return ntohs(src_port); }
    uint16_t dest_port() const { return ntohs(dst_port); }
    uint16_t data_length() const { return ntohs(length) - 8; }
};

static_assert(sizeof(UDPHeader) == 8, "UDPHeader must be 8 bytes");

// ============================================================================
// UDP 伪头部 (用于校验和计算)
// ============================================================================

struct UDPPseudoHeader {
    uint32_t src_addr;    // 源 IP (网络字节序)
    uint32_t dst_addr;    // 目标 IP (网络字节序)
    uint8_t  zero;        // 保留，必须为 0
    uint8_t  protocol;    // 协议号 (17)
    uint16_t udp_length;  // UDP 长度 (网络字节序)
};

static_assert(sizeof(UDPPseudoHeader) == 12, "UDPPseudoHeader must be 12 bytes");

// ============================================================================
// UDP 数据报 (解析后，主机字节序)
// ============================================================================

struct UDPDatagram {
    // 来源信息 (从 IP 层获取)
    uint32_t src_addr;
    uint32_t dst_addr;

    // UDP 头部字段
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;

    // 数据
    const uint8_t *data;
    size_t data_length;
};

// ============================================================================
// UDP Socket 回调
// ============================================================================

// 收到数据时的回调函数
// 参数: 源IP, 源端口, 数据指针, 数据长度
using UDPReceiveCallback = std::function<void(uint32_t src_ip, uint16_t src_port,
                                              const uint8_t *data, size_t len)>;

// ============================================================================
// UDPLayer - UDP 层
// ============================================================================

class UDPLayer : public IProtocolHandler {
public:
    explicit UDPLayer(IPv4Layer &ip_layer);

    // 实现 IProtocolHandler接口
    void handle(const IPv4Packet &pkt) override;

    // ═══════════════════════════════════════════════════════════════════
    // Socket 操作
    // ═══════════════════════════════════════════════════════════════════

    /**
     * @brief 绑定端口
     * @param port 本地端口 (0 = 自动分配)
     * @param callback 收到数据时的回调
     * @return 绑定的端口号，0 表示失败
     */
    uint16_t bind(uint16_t port, UDPReceiveCallback callback);

    /**
     * @brief 解绑端口
     * @param port 要解绑的端口
     */
    void unbind(uint16_t port);

    /**
     * @brief 发送 UDP 数据
     * @param dst_ip 目标 IP
     * @param dst_port 目标端口
     * @param src_port 源端口 (必须已绑定)
     * @param data 数据
     * @param len 数据长度
     * @return 发送的字节数，-1 表示失败
     */
    ssize_t sendto(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                   const uint8_t *data, size_t len);

private:
    // 解析 UDP 数据报
    std::optional<UDPDatagram> parse(const IPv4Packet &pkt);

    // 验证校验和
    bool verify_checksum(const IPv4Packet &pkt);

    // 计算校验和
    uint16_t compute_udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                  const uint8_t *udp_packet, size_t udp_len);

    // 分配临时端口
    uint16_t allocate_ephemeral_port();

    IPv4Layer &_ip_layer;
    std::unordered_map<uint16_t, UDPReceiveCallback> _sockets;
    uint16_t _next_ephemeral_port = 49152; // 临时端口范围: 49152-65535
};

#endif