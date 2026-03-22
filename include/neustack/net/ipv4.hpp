#ifndef NEUSTACK_NET_IPV4_HPP
#define NEUSTACK_NET_IPV4_HPP

#include "neustack/common/platform.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "neustack/hal/device.hpp"
#include "neustack/net/protocol_handler.hpp"

namespace neustack {

// ============================================================================
// Protocol Numbers
// ============================================================================

enum class IPProtocol : uint8_t {
    ICMP = 1,
    TCP  = 6,
    UDP  = 17,
};

// ============================================================================
// IPv4Header - 网络字节序的 IPv4 头部 (用于直接映射内存)
// ============================================================================

struct IPv4Header {
    uint8_t  version_ihl;     // 高 4 位: version, 低 4 位: IHL
    uint8_t  dscp_ecn;        // 高 6 位: DSCP, 低 2 位: ECN
    uint16_t total_length;    // 整个报文长度 (网络字节序)
    uint16_t identification;  // 分片标识
    uint16_t flags_fragment;  // 高 3 位: flags, 低 13 位: fragment offset
    uint8_t  ttl;             // Time to Live
    uint8_t  protocol;        // 上层协议号 (1=ICMP, 6=TCP, 17=UDP)
    uint16_t checksum;        // 头部校验和
    uint32_t src_addr;        // 源 IP 地址 (网络字节序)
    uint32_t dst_addr;        // 目标 IP 地址 (网络字节序)

    // 辅助方法
    uint8_t version() const { return version_ihl >> 4; }
    uint8_t ihl() const { return version_ihl & 0x0F; }
    size_t header_length() const { return ihl() * 4; }
    size_t payload_length() const {
        uint16_t tl = ntohs(total_length);
        size_t hl = header_length();
        return (tl >= hl) ? (tl - hl) : 0;
    }

    uint8_t flags() const { return ntohs(flags_fragment) >> 13; }
    uint16_t fragment_offset() const { return ntohs(flags_fragment) & 0x1FFF; }

    bool dont_fragment() const { return flags() & 0x02; }
    bool more_fragments() const { return flags() & 0x01; }
};

static_assert(sizeof(IPv4Header) == 20, "IPv4Header must be 20 bytes");

// ============================================================================
// IPv4Packet - 解析后的 IPv4 报文 (主机字节序，便于处理)
// ============================================================================

struct IPv4Packet {
    // 头部字段 (主机字节序)
    uint8_t  version;
    uint8_t  ihl;
    uint8_t  dscp;
    uint8_t  ecn;
    uint16_t total_length;
    uint16_t identification;
    bool     dont_fragment;
    bool     more_fragments;
    uint16_t fragment_offset;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_addr;
    uint32_t dst_addr;

    // Payload
    const uint8_t* payload;
    size_t payload_length;

    // 原始数据引用 (用于转发等场景)
    const uint8_t* raw_data;
    size_t raw_length;
};

// ============================================================================
// IPv4Parser - IPv4 报文解析器
// ============================================================================

class IPv4Parser {
public:
    /**
     * @brief 解析 IPv4 报文
     * @param data 原始数据
     * @param len 数据长度
     * @return 解析后的报文，失败返回 nullopt
     */
    static std::optional<IPv4Packet> parse(const uint8_t* data, size_t len);

    /**
     * @brief 验证 IPv4 报文
     * @param data 原始数据
     * @param len 数据长度
     * @return 错误信息，nullptr 表示无错误
     */
    static const char* validate(const uint8_t* data, size_t len);
};

// ============================================================================
// IPv4Builder - IPv4 报文构造器 (Builder 模式)
// ============================================================================

class IPv4Builder {
public:
    IPv4Builder& set_dscp(uint8_t dscp) { _dscp = dscp; return *this; }
    IPv4Builder& set_ecn(uint8_t ecn) { _ecn = ecn; return *this; }
    IPv4Builder& set_identification(uint16_t id) { _identification = id; return *this; }
    IPv4Builder& set_dont_fragment(bool df) { _dont_fragment = df; return *this; }
    IPv4Builder& set_ttl(uint8_t ttl) { _ttl = ttl; return *this; }
    IPv4Builder& set_protocol(uint8_t proto) { _protocol = proto; return *this; }
    IPv4Builder& set_src(uint32_t addr) { _src_addr = addr; return *this; }
    IPv4Builder& set_dst(uint32_t addr) { _dst_addr = addr; return *this; }

    IPv4Builder& set_payload(const uint8_t* data, size_t len) {
        _payload = data;
        _payload_len = len;
        return *this;
    }

    /**
     * @brief 构建 IPv4 报文
     * @param buffer 输出缓冲区
     * @param buffer_len 缓冲区长度
     * @return 实际写入长度，-1 表示失败
     */
    ssize_t build(uint8_t* buffer, size_t buffer_len) const;

    /**
     * @brief 只写 IPv4 header，不拷贝 payload
     * @param buffer header 写到这里
     * @param buffer_len 缓冲区长度
     * @param payload_len payload 长度 (用于计算 total_length)
     * @return header 长度 (20)，-1 表示失败
     */
    ssize_t build_header_only(uint8_t* buffer, size_t buffer_len,
                              size_t payload_len) const;

private:
    uint8_t  _dscp = 0;
    uint8_t  _ecn = 0;
    uint16_t _identification = 0;
    bool     _dont_fragment = true;
    uint8_t  _ttl = 64;
    uint8_t  _protocol = 0;
    uint32_t _src_addr = 0;
    uint32_t _dst_addr = 0;
    const uint8_t* _payload = nullptr;
    size_t _payload_len = 0;
};

// ============================================================================
// IPv4Layer - IPv4 层管理类
// ============================================================================

class IPv4Layer {
public:
    /**
     * @brief 构造 IPv4 层
     * @param device 网络设备引用
     */
    explicit IPv4Layer(NetDevice& device);

    /**
     * @brief 注册协议处理器
     * @param protocol 协议号 (如 ICMP=1, TCP=6)
     * @param handler 处理器指针
     */
    void register_handler(uint8_t protocol, IProtocolHandler* handler);

    /**
     * @brief 获取协议处理器
     * @param protocol 协议号
     * @return 处理器指针，未注册返回 nullptr
     */
    IProtocolHandler* get_handler(uint8_t protocol) const;

    /**
     * @brief 处理收到的数据包
     * @param data 原始数据
     * @param len 数据长度
     */
    void on_receive(const uint8_t* data, size_t len);

    /**
     * @brief 发送 IP 数据包
     * @param dst 目标 IP (主机字节序)
     * @param protocol 协议号
     * @param payload 负载数据
     * @param len 负载长度
     * @return 发送字节数，-1 表示失败
     */
    ssize_t send(uint32_t dst, uint8_t protocol, const uint8_t* payload, size_t len);

    // IP 地址管理
    void set_local_ip(uint32_t ip) { _local_ip = ip; }
    uint32_t local_ip() const { return _local_ip; }

    // MTU 管理
    void set_mtu(uint16_t mtu) { _mtu = mtu; }
    uint16_t mtu() const { return _mtu; }

private:
    NetDevice& _device;
    uint32_t _local_ip = 0;
    uint16_t _mtu = 1500;  // 默认 MTU
    std::unordered_map<uint8_t, IProtocolHandler*> _handlers;
    uint16_t _next_id = 0;  // 用于 Identification 字段
};

} // namespace neustack

#endif // NEUSTACK_NET_IPV4_HPP
