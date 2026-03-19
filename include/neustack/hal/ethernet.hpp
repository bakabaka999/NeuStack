#ifndef NEUSTACK_HAL_ETHERNET_HPP
#define NEUSTACK_HAL_ETHERNET_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace neustack {

static constexpr size_t   ETH_HLEN   = 14;
static constexpr uint16_t ETH_P_IP   = 0x0800;
static constexpr uint16_t ETH_P_ARP  = 0x0806;
static constexpr uint16_t ETH_P_IPV6 = 0x86DD;

/**
 * Ethernet Header (14 bytes)
 *
 * 直接映射到网络字节序的 L2 帧头。
 */
struct EthernetHeader {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype_be; // 网络字节序

    /** 获取主机字节序的 EtherType */
    uint16_t ethertype() const {
        return (ethertype_be >> 8) | (ethertype_be << 8);
    }
} __attribute__((packed));

/**
 * 解析 Ethernet 帧，剥离 L2 头部
 *
 * @param frame      原始帧数据指针
 * @param frame_len  帧总长度
 * @param payload    [out] L3 payload 指针
 * @param payload_len [out] L3 payload 长度
 * @return EtherType (主机字节序)，帧太短返回 0
 */
inline uint16_t parse_ethernet(const uint8_t* frame, uint32_t frame_len,
                               const uint8_t** payload, uint32_t* payload_len) {
    if (frame_len < ETH_HLEN) return 0;
    auto* hdr = reinterpret_cast<const EthernetHeader*>(frame);
    *payload = frame + ETH_HLEN;
    *payload_len = frame_len - ETH_HLEN;
    return hdr->ethertype();
}

/**
 * 在 buffer 前面构建 Ethernet 头部
 *
 * 调用者需要确保 buf 前面有 ETH_HLEN 字节的空间 (headroom)。
 *
 * @param buf        L3 数据起始位置
 * @param dst_mac    目标 MAC (6 bytes)
 * @param src_mac    源 MAC (6 bytes)
 * @param ethertype  EtherType (主机字节序，如 ETH_P_IP)
 * @return 完整帧的起始指针 (buf - ETH_HLEN)
 */
inline uint8_t* build_ethernet(uint8_t* buf,
                                const uint8_t* dst_mac,
                                const uint8_t* src_mac,
                                uint16_t ethertype) {
    auto* hdr = reinterpret_cast<EthernetHeader*>(buf - ETH_HLEN);
    memcpy(hdr->dst, dst_mac, 6);
    memcpy(hdr->src, src_mac, 6);
    hdr->ethertype_be = static_cast<uint16_t>((ethertype >> 8) | (ethertype << 8));
    return reinterpret_cast<uint8_t*>(hdr);
}

} // namespace neustack

#endif // NEUSTACK_HAL_ETHERNET_HPP
