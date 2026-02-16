#ifndef NEUSTACK_FIREWALL_PACKET_EVENT_HPP
#define NEUSTACK_FIREWALL_PACKET_EVENT_HPP

#include <cstdint>

namespace neustack {

/**
 * 数据包事件 - 防火墙处理的核心数据结构
 *
 * 设计原则:
 * - 零拷贝: payload_ptr 直接指向原始缓冲区
 * - 池化友好: 固定大小，无动态分配
 * - 紧凑布局: 32 字节，缓存行友好
 *
 * 生命周期: 仅在 FirewallEngine::inspect() 调用期间有效
 */
struct PacketEvent {
    // ─── 网络层信息 (8 bytes) ───
    uint32_t src_ip;              // 源 IP (网络字节序)
    uint32_t dst_ip;              // 目标 IP (网络字节序)

    // ─── 传输层信息 (6 bytes) ───
    uint16_t src_port;            // 源端口 (主机字节序, 0 = N/A)
    uint16_t dst_port;            // 目标端口 (主机字节序, 0 = N/A)
    uint8_t  protocol;            // IP 协议号 (6=TCP, 17=UDP, 1=ICMP)
    uint8_t  tcp_flags;           // TCP 标志位 (仅 TCP 有效)

    // ─── 包元信息 (8 bytes) ───
    uint16_t total_len;           // IP 包总长度
    uint16_t payload_len;         // 传输层 payload 长度
    uint32_t _reserved;           // 保留，对齐用

    // ─── 时间戳 (8 bytes) ───
    uint64_t timestamp_us;        // 微秒时间戳

    // ─── 零拷贝指针 (不计入结构大小比较，仅用于深度检测) ───
    const uint8_t* raw_packet;    // 原始包指针 (生命周期由调用者保证)

    // ─── 辅助方法 ───

    bool is_tcp() const { return protocol == 6; }
    bool is_udp() const { return protocol == 17; }
    bool is_icmp() const { return protocol == 1; }

    // TCP 标志判断
    bool is_syn() const { return is_tcp() && (tcp_flags & 0x02); }
    bool is_ack() const { return is_tcp() && (tcp_flags & 0x10); }
    bool is_fin() const { return is_tcp() && (tcp_flags & 0x01); }
    bool is_rst() const { return is_tcp() && (tcp_flags & 0x04); }

    // SYN-only (不带 ACK) - SYN Flood 检测用
    bool is_syn_only() const { return is_tcp() && (tcp_flags & 0x02) && !(tcp_flags & 0x10); }
};

// 验证结构大小
static_assert(sizeof(PacketEvent) == 40 || sizeof(PacketEvent) == 48, 
              "PacketEvent should be 40 bytes (32-bit) or 48 bytes (64-bit)");

} // namespace neustack

#endif // NEUSTACK_FIREWALL_PACKET_EVENT_HPP
