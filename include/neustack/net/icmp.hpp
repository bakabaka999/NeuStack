#ifndef NEUSTACK_NET_ICMP_HPP
#define NEUSTACK_NET_ICMP_HPP

#include <arpa/inet.h>

#include "neustack/net/protocol_handler.hpp"
#include "neustack/net/ipv4.hpp"

namespace neustack {

// ICMP类型定义
enum class ICMPType : uint8_t
{ // 列了常见的几个
    EchoReply           = 0,    // 回显应答
    DestUnreachable     = 3,    // 目的不可达
    Redirect            = 5,    // 重定向
    EchoRequest         = 8,    // 回显请求
    TimeExceeded        = 11,   // 超时
    ParameterProblem    = 12,   // 参数错误
};

// Destination Unreachable 代码
enum class ICMPUnreachCode : uint8_t {
    NetUnreachable      = 0,
    HostUnreachable     = 1,
    ProtocolUnreachable = 2,
    PortUnreachable     = 3,
    FragmentNeeded      = 4,
    SourceRouteFailed   = 5,
    // ...
};

// Time Exceeded 代码
enum class ICMPTimeExCode : uint8_t {
    TTLExceeded         = 0,
    FragReassembly      = 1,
};

// ICMP通用头部
struct ICMPHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
};

// ICMP Echo头部
struct ICMPEcho {
    uint16_t identifier;
    uint16_t sequence;
    // 后面是可变长度对data
};

// ICMP Error头部
struct ICMPError {
    uint16_t unused;        // Type 3 Code 4 时是 MTU 低 16 位
    uint16_t next_hop_mtu;  // Type 3 Code 4 时使用
    // 原始 IP 头部 + 8 字节 follows...
};

static_assert(sizeof(ICMPHeader) == 4, "ICMPHeader must be 4 bytes");
static_assert(sizeof(ICMPEcho) == 4, "ICMPEcho must be 4 bytes");
static_assert(sizeof(ICMPError) == 4, "ICMPError must be 4 bytes");

class ICMPHandler : public IProtocolHandler {
public:
    explicit ICMPHandler(IPv4Layer &ip_layer) : _ip_layer(ip_layer) {}

    // 实现 IProtocolHandler 接口
    void handle(const IPv4Packet &pkt) override;

    // 下面的方法供其他层调用，用于发送ICMP

    // 错误消息
    void send_dest_unreachable(ICMPUnreachCode code,
                               const IPv4Packet &original_pkt);

    void send_time_exceeded(ICMPTimeExCode code,
                            const IPv4Packet &original_pkt);

    // 主动发送
    /**
     * @brief 发送 Echo Request (ping)
     * @param dst_ip 目标 IP (主机字节序)
     * @param identifier 标识符 (通常用 PID)
     * @param sequence 序列号
     * @param data 可选的附加数据
     * @param data_len 附加数据长度
     */
    void send_echo_request(uint32_t dst_ip, uint16_t identifier, uint16_t sequence,
                           const uint8_t *data = nullptr, size_t data_len = 0);

private:
    void handle_echo_request(const IPv4Packet &pkt);
    void handle_echo_reply(const IPv4Packet &pkt);

    void send_error(uint8_t type, uint8_t code,
                    uint32_t extra, // unused/mtu字段
                    const IPv4Packet &original_pkt);

    IPv4Layer &_ip_layer;
};

} // namespace neustack

#endif // NEUSTACK_NET_ICMP_HPP
