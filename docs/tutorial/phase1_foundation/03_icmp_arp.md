# ICMP 与 ARP 实现教程

## 第一部分：ICMP

### 1. ICMP 概述

ICMP (Internet Control Message Protocol) 是 IP 协议的核心辅助协议，承担两大职责：

1. **网络诊断** - ping (Echo Request/Reply)
2. **错误报告** - 告知发送方数据包无法送达的原因

```
┌─────────────────────────────────────────────────────────────────┐
│                         应用层                                   │
│    ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐          │
│    │  ping   │  │  HTTP   │  │   DNS   │  │   ...   │          │
│    └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘          │
└─────────┼────────────┼────────────┼────────────┼────────────────┘
          │            │            │            │
          ▼            ▼            ▼            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       传输层 (TCP/UDP)                           │
│                                                                  │
│    收到 ICMP 错误后，通知对应的 socket                            │
└─────────────────────┬───────────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────────┐
│                         ICMP 层                                  │
│  ┌──────────────┐  ┌────────────────────────────────────────┐   │
│  │ Echo Request │  │           错误消息处理                  │   │
│  │   → Reply    │  │  Type 3: Destination Unreachable       │   │
│  └──────────────┘  │  Type 11: Time Exceeded                │   │
│                    │  Type 12: Parameter Problem            │   │
│                    └────────────────────────────────────────┘   │
└─────────────────────┬───────────────────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────────────────┐
│                         IPv4 层                                  │
│                     Protocol = 1 (ICMP)                          │
└─────────────────────────────────────────────────────────────────┘
```

---

### 2. ICMP 完整类型表

#### 2.1 你需要实现的类型

| Type | Code | 名称 | 触发场景 | 谁发送 |
|------|------|------|----------|--------|
| **0** | 0 | Echo Reply | 收到 Echo Request | ICMP 层 |
| **3** | 0 | Net Unreachable | 路由表无匹配 | IP 层 (如实现路由) |
| **3** | 1 | Host Unreachable | ARP 解析失败 | IP 层 (如实现 ARP) |
| **3** | 2 | Protocol Unreachable | IP 层收到未知协议 | IP 层 |
| **3** | 3 | Port Unreachable | UDP 端口无监听 | **UDP 层** |
| **3** | 4 | Fragmentation Needed | 需分片但 DF=1 | IP 层 |
| **8** | 0 | Echo Request | ping 命令 | 应用层 |
| **11** | 0 | TTL Exceeded | TTL 减到 0 | IP 层 |
| **11** | 1 | Fragment Reassembly Timeout | 分片重组超时 | IP 层 |

#### 2.2 完整类型参考 (了解即可)

| Type | 名称 | 说明 |
|------|------|------|
| 0 | Echo Reply | ping 回复 |
| 3 | Destination Unreachable | 目标不可达 (最常用的错误) |
| 4 | Source Quench | 拥塞控制 (已废弃) |
| 5 | Redirect | 路由重定向 |
| 8 | Echo Request | ping 请求 |
| 9 | Router Advertisement | 路由器通告 |
| 10 | Router Solicitation | 路由器请求 |
| 11 | Time Exceeded | 超时 |
| 12 | Parameter Problem | 头部参数问题 |
| 13/14 | Timestamp | 时间戳 (很少用) |
| 15/16 | Information | 信息请求 (已废弃) |
| 17/18 | Address Mask | 地址掩码 (已废弃) |

---

### 3. ICMP 报文格式

#### 3.1 通用头部 (所有类型共享)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Type      |     Code      |          Checksum             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Type-specific Data                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### 3.2 Echo Request/Reply (Type 8/0)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Type (8/0)   |    Code (0)   |          Checksum             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Identifier          |        Sequence Number        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Data ...                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Identifier**: 标识发送进程 (通常是 PID)
- **Sequence Number**: 序列号，每次 ping 递增
- **Data**: 任意数据，Reply 时必须原样返回

#### 3.3 Destination Unreachable (Type 3)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Type (3)    |     Code      |          Checksum             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           unused              |         Next-Hop MTU          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      原始 IP 头部 (20 字节) + 原始数据前 8 字节                  |
|      (共 28 字节，用于发送方识别是哪个包出了问题)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**为什么要包含原始数据的前 8 字节？**
- TCP/UDP 头部的前 8 字节包含：源端口 (2) + 目标端口 (2) + 序列号/长度 (4)
- 这样发送方可以定位是哪个连接/socket 出了问题

**Type 3 的 Code 值：**

| Code | 名称 | 触发场景 |
|------|------|----------|
| 0 | Net Unreachable | 路由表没有到目标网络的路由 |
| 1 | Host Unreachable | 能到达网络，但找不到具体主机 (ARP 失败) |
| 2 | Protocol Unreachable | IP 层收到的 protocol 字段没有对应处理器 |
| 3 | Port Unreachable | **UDP 目标端口没有监听** (最常见!) |
| 4 | Fragmentation Needed | 包太大需要分片，但 DF=1 禁止分片 |
| 5 | Source Route Failed | 源路由失败 |
| 6-15 | 其他 | 较少使用 |

#### 3.4 Time Exceeded (Type 11)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Type (11)   |     Code      |          Checksum             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          unused                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      原始 IP 头部 (20 字节) + 原始数据前 8 字节                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Code | 名称 | 触发场景 |
|------|------|----------|
| 0 | TTL Exceeded in Transit | TTL 减到 0 (traceroute 利用这个) |
| 1 | Fragment Reassembly Timeout | 分片重组超时 |

---

### 4. ICMP 消息流向详解

#### 4.1 Echo Request/Reply (ping)

```
┌─────────────────────────────────────────────────────────────────┐
│ 主机 A (192.168.1.1)                主机 B (192.168.1.2)        │
│                                                                  │
│  ping 192.168.1.2                                               │
│       │                                                         │
│       ▼                                                         │
│  ICMP Echo Request ─────────────────────────────────────────►   │
│  Type=8, Code=0                                                 │
│  ID=1234, Seq=1                                                 │
│  Data="hello"                                                   │
│                                                                  │
│                         ◄───────────────────────── ICMP Echo Reply
│                                                   Type=0, Code=0 │
│                                                   ID=1234, Seq=1 │
│                                                   Data="hello"   │
│       │                                                         │
│       ▼                                                         │
│  收到回复! RTT=0.5ms                                             │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.2 Port Unreachable (UDP)

这是 **UDP 层** 发送的 ICMP 错误，**不是** ICMP 层发送的！

```
┌─────────────────────────────────────────────────────────────────┐
│ 客户端                                         服务器           │
│                                                                  │
│  UDP 发送到端口 5000                                             │
│       │                                                         │
│       ▼                                                         │
│  UDP Datagram ───────────────────────────────────────────────►  │
│  Dst Port: 5000                                                 │
│                                                                  │
│                                        端口 5000 无人监听        │
│                                               │                 │
│                                               ▼                 │
│                         ◄─────────────────── ICMP Dest Unreachable
│                                              Type=3, Code=3     │
│                                              (Port Unreachable) │
│                                              原始 IP+UDP 头部    │
│       │                                                         │
│       ▼                                                         │
│  收到 ICMP 错误                                                  │
│  上报给应用: "Connection refused"                                │
└─────────────────────────────────────────────────────────────────┘
```

#### 4.3 Protocol Unreachable

IP 层收到未知协议时发送：

```
┌─────────────────────────────────────────────────────────────────┐
│ 发送方                                         接收方           │
│                                                                  │
│  IP 包，Protocol=99                                              │
│  (某个没人用的协议)                                               │
│       │                                                         │
│       ▼                                                         │
│  IP Packet ──────────────────────────────────────────────────►  │
│  Protocol: 99                                                   │
│                                                                  │
│                                        Protocol 99 没有处理器   │
│                                               │                 │
│                                               ▼                 │
│                         ◄─────────────────── ICMP Dest Unreachable
│                                              Type=3, Code=2     │
│                                              (Protocol Unreachable)
└─────────────────────────────────────────────────────────────────┘
```

#### 4.4 TTL Exceeded (traceroute 原理)

```
┌─────────────────────────────────────────────────────────────────┐
│ traceroute 192.168.2.100                                        │
│                                                                  │
│  发送 TTL=1 的包                                                 │
│       │                                                         │
│       ▼                                                         │
│  ────────────►  路由器 1                                         │
│                 TTL-- → 0                                       │
│                 丢弃，发送 ICMP                                  │
│  ◄────────────  Type=11, Code=0 (TTL Exceeded)                  │
│  Hop 1: 192.168.1.1                                             │
│                                                                  │
│  发送 TTL=2 的包                                                 │
│       │                                                         │
│       ▼                                                         │
│  ────────────►  路由器 1 ────────────►  路由器 2                 │
│                 TTL--=1                 TTL-- → 0               │
│                                         丢弃，发送 ICMP         │
│  ◄──────────────────────────────────  Type=11, Code=0           │
│  Hop 2: 192.168.1.2                                             │
│                                                                  │
│  ... 继续递增 TTL 直到到达目的地 ...                              │
└─────────────────────────────────────────────────────────────────┘
```

---

### 5. 数据结构设计

#### 5.1 文件位置

```
include/neustack/net/icmp.hpp
src/net/icmp.cpp
```

#### 5.2 枚举定义

```cpp
// ICMP 类型
enum class ICMPType : uint8_t {
    EchoReply           = 0,
    DestUnreachable     = 3,
    SourceQuench        = 4,   // 已废弃
    Redirect            = 5,
    EchoRequest         = 8,
    TimeExceeded        = 11,
    ParameterProblem    = 12,
};

// Destination Unreachable 代码
enum class ICMPUnreachCode : uint8_t {
    NetUnreachable      = 0,
    HostUnreachable     = 1,
    ProtocolUnreachable = 2,
    PortUnreachable     = 3,
    FragmentNeeded      = 4,
    SourceRouteFailed   = 5,
};

// Time Exceeded 代码
enum class ICMPTimeExCode : uint8_t {
    TTLExceeded         = 0,
    FragReassembly      = 1,
};
```

#### 5.3 头部结构

```cpp
// ICMP 通用头部 (所有类型)
struct ICMPHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
};

// Echo Request/Reply 特有部分
struct ICMPEcho {
    uint16_t identifier;   // 网络字节序
    uint16_t sequence;     // 网络字节序
    // data follows...
};

// Destination Unreachable / Time Exceeded 特有部分
struct ICMPError {
    uint16_t unused;       // Type 3 Code 4 时是 MTU 低 16 位
    uint16_t next_hop_mtu; // Type 3 Code 4 时使用
    // 原始 IP 头部 + 8 字节 follows...
};

static_assert(sizeof(ICMPHeader) == 4, "ICMPHeader must be 4 bytes");
static_assert(sizeof(ICMPEcho) == 4, "ICMPEcho must be 4 bytes");
static_assert(sizeof(ICMPError) == 4, "ICMPError must be 4 bytes");
```

---

### 6. ICMPHandler 完整设计

#### 6.1 类接口

```cpp
// include/neustack/net/icmp.hpp

class ICMPHandler : public IProtocolHandler {
public:
    explicit ICMPHandler(IPv4Layer& ip_layer);

    // 实现 IProtocolHandler 接口
    void handle(const IPv4Packet& pkt) override;

    // ═══════════════════════════════════════════════════════════════
    // 以下方法供其他层调用，用于发送 ICMP 错误
    // ═══════════════════════════════════════════════════════════════

    /**
     * @brief 发送 Destination Unreachable
     * @param code 错误代码 (见 ICMPUnreachCode)
     * @param original_pkt 触发错误的原始 IPv4 包
     *
     * 使用场景:
     * - Code 2: IP 层收到未知协议 → ip_layer.on_receive() 调用
     * - Code 3: UDP 端口无监听 → udp_layer.on_receive() 调用
     * - Code 4: 需分片但 DF=1 → ip_layer.send() 调用
     */
    void send_dest_unreachable(ICMPUnreachCode code,
                                const IPv4Packet& original_pkt);

    /**
     * @brief 发送 Time Exceeded
     * @param code 错误代码 (0=TTL, 1=分片重组)
     * @param original_pkt 触发错误的原始 IPv4 包
     *
     * 使用场景:
     * - Code 0: TTL 减到 0 → ip_layer.on_receive() 调用
     * - Code 1: 分片重组超时 → 分片重组模块调用
     */
    void send_time_exceeded(ICMPTimeExCode code,
                             const IPv4Packet& original_pkt);

private:
    void handle_echo_request(const IPv4Packet& pkt);
    void handle_echo_reply(const IPv4Packet& pkt);

    void send_error(uint8_t type, uint8_t code,
                    uint32_t extra,  // unused/mtu 字段
                    const IPv4Packet& original_pkt);

    IPv4Layer& ip_layer_;
};
```

#### 6.2 实现

```cpp
// src/net/icmp.cpp

#include "neustack/net/icmp.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/ip_addr.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

ICMPHandler::ICMPHandler(IPv4Layer& ip_layer)
    : ip_layer_(ip_layer) {}

void ICMPHandler::handle(const IPv4Packet& pkt) {
    // 最小长度检查
    if (pkt.payload_length < sizeof(ICMPHeader)) {
        return;
    }

    auto* hdr = reinterpret_cast<const ICMPHeader*>(pkt.payload);

    // 验证校验和
    if (!verify_checksum(pkt.payload, pkt.payload_length)) {
        std::printf("ICMP: checksum error\n");
        return;
    }

    // 根据类型分发
    switch (static_cast<ICMPType>(hdr->type)) {
        case ICMPType::EchoRequest:
            handle_echo_request(pkt);
            break;

        case ICMPType::EchoReply:
            handle_echo_reply(pkt);
            break;

        case ICMPType::DestUnreachable:
            // 收到错误通知，可以通知上层 (TCP/UDP)
            std::printf("ICMP: Dest Unreachable from %s, code=%u\n",
                ip_to_string(pkt.src_addr).c_str(), hdr->code);
            // TODO: 解析原始包，通知对应的 socket
            break;

        case ICMPType::TimeExceeded:
            std::printf("ICMP: Time Exceeded from %s, code=%u\n",
                ip_to_string(pkt.src_addr).c_str(), hdr->code);
            break;

        default:
            std::printf("ICMP: unhandled type %u\n", hdr->type);
            break;
    }
}

void ICMPHandler::handle_echo_request(const IPv4Packet& pkt) {
    if (pkt.payload_length < sizeof(ICMPHeader) + sizeof(ICMPEcho)) {
        return;
    }

    auto* hdr = reinterpret_cast<const ICMPHeader*>(pkt.payload);
    auto* echo = reinterpret_cast<const ICMPEcho*>(hdr + 1);

    // Echo data
    const uint8_t* data = reinterpret_cast<const uint8_t*>(echo + 1);
    size_t data_len = pkt.payload_length - sizeof(ICMPHeader) - sizeof(ICMPEcho);

    std::printf("ICMP Echo Request from %s, id=%u, seq=%u\n",
        ip_to_string(pkt.src_addr).c_str(),
        ntohs(echo->identifier),
        ntohs(echo->sequence));

    // 构造 Echo Reply
    size_t reply_len = pkt.payload_length;  // 和请求一样长
    uint8_t reply_buf[1500];

    if (reply_len > sizeof(reply_buf)) {
        return;
    }

    auto* reply_hdr = reinterpret_cast<ICMPHeader*>(reply_buf);
    auto* reply_echo = reinterpret_cast<ICMPEcho*>(reply_hdr + 1);
    uint8_t* reply_data = reinterpret_cast<uint8_t*>(reply_echo + 1);

    reply_hdr->type = static_cast<uint8_t>(ICMPType::EchoReply);
    reply_hdr->code = 0;
    reply_hdr->checksum = 0;

    // Identifier 和 Sequence 原样返回
    reply_echo->identifier = echo->identifier;
    reply_echo->sequence = echo->sequence;

    // Data 原样返回
    if (data_len > 0) {
        std::memcpy(reply_data, data, data_len);
    }

    // 计算校验和
    reply_hdr->checksum = compute_checksum(reply_buf, reply_len);

    // 发送
    ip_layer_.send(pkt.src_addr,
                   static_cast<uint8_t>(IPProtocol::ICMP),
                   reply_buf, reply_len);

    std::printf("ICMP Echo Reply sent to %s\n",
        ip_to_string(pkt.src_addr).c_str());
}

void ICMPHandler::handle_echo_reply(const IPv4Packet& pkt) {
    if (pkt.payload_length < sizeof(ICMPHeader) + sizeof(ICMPEcho)) {
        return;
    }

    auto* hdr = reinterpret_cast<const ICMPHeader*>(pkt.payload);
    auto* echo = reinterpret_cast<const ICMPEcho*>(hdr + 1);

    std::printf("ICMP Echo Reply from %s, id=%u, seq=%u\n",
        ip_to_string(pkt.src_addr).c_str(),
        ntohs(echo->identifier),
        ntohs(echo->sequence));

    // TODO: 通知等待 ping 回复的应用
}

// ═══════════════════════════════════════════════════════════════════
// 错误消息发送
// ═══════════════════════════════════════════════════════════════════

void ICMPHandler::send_dest_unreachable(ICMPUnreachCode code,
                                         const IPv4Packet& original_pkt) {
    // 检查: 不对 ICMP 错误消息回复 ICMP 错误
    if (original_pkt.protocol == static_cast<uint8_t>(IPProtocol::ICMP)) {
        auto* icmp_hdr = reinterpret_cast<const ICMPHeader*>(original_pkt.payload);
        if (icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoRequest) &&
            icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoReply)) {
            // 原始包是 ICMP 错误消息，不回复
            return;
        }
    }

    uint32_t extra = 0;  // unused 字段
    // Code 4 (Fragmentation Needed) 时可以设置 MTU
    // if (code == ICMPUnreachCode::FragmentNeeded) {
    //     extra = htons(mtu);  // 低 16 位是 MTU
    // }

    send_error(static_cast<uint8_t>(ICMPType::DestUnreachable),
               static_cast<uint8_t>(code),
               extra,
               original_pkt);

    std::printf("ICMP Dest Unreachable (code=%u) sent to %s\n",
        static_cast<uint8_t>(code),
        ip_to_string(original_pkt.src_addr).c_str());
}

void ICMPHandler::send_time_exceeded(ICMPTimeExCode code,
                                      const IPv4Packet& original_pkt) {
    // 同样检查不对 ICMP 错误回复
    if (original_pkt.protocol == static_cast<uint8_t>(IPProtocol::ICMP)) {
        auto* icmp_hdr = reinterpret_cast<const ICMPHeader*>(original_pkt.payload);
        if (icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoRequest) &&
            icmp_hdr->type != static_cast<uint8_t>(ICMPType::EchoReply)) {
            return;
        }
    }

    send_error(static_cast<uint8_t>(ICMPType::TimeExceeded),
               static_cast<uint8_t>(code),
               0,  // unused
               original_pkt);

    std::printf("ICMP Time Exceeded (code=%u) sent to %s\n",
        static_cast<uint8_t>(code),
        ip_to_string(original_pkt.src_addr).c_str());
}

void ICMPHandler::send_error(uint8_t type, uint8_t code,
                              uint32_t extra,
                              const IPv4Packet& original_pkt) {
    // 错误消息格式:
    // ICMP Header (4) + Extra (4) + Original IP Header + 8 bytes

    // 复制原始 IP 头部 + 最多 8 字节 payload
    size_t orig_ip_hdr_len = original_pkt.ihl * 4;
    size_t orig_data_len = std::min(original_pkt.payload_length, size_t(8));
    size_t orig_copy_len = orig_ip_hdr_len + orig_data_len;

    size_t icmp_len = sizeof(ICMPHeader) + sizeof(ICMPError) + orig_copy_len;
    uint8_t icmp_buf[256];

    if (icmp_len > sizeof(icmp_buf)) {
        return;
    }

    auto* hdr = reinterpret_cast<ICMPHeader*>(icmp_buf);
    auto* err = reinterpret_cast<ICMPError*>(hdr + 1);
    uint8_t* orig = reinterpret_cast<uint8_t*>(err + 1);

    // ICMP 头部
    hdr->type = type;
    hdr->code = code;
    hdr->checksum = 0;

    // Extra 字段 (unused / MTU)
    err->unused = (extra >> 16) & 0xFFFF;
    err->next_hop_mtu = extra & 0xFFFF;

    // 复制原始 IP 头部
    std::memcpy(orig, original_pkt.raw_data, orig_ip_hdr_len);

    // 复制原始 payload 的前 8 字节
    if (orig_data_len > 0) {
        std::memcpy(orig + orig_ip_hdr_len, original_pkt.payload, orig_data_len);
    }

    // 计算校验和
    hdr->checksum = compute_checksum(icmp_buf, icmp_len);

    // 发送给原始包的发送者
    ip_layer_.send(original_pkt.src_addr,
                   static_cast<uint8_t>(IPProtocol::ICMP),
                   icmp_buf, icmp_len);
}
```

---

### 7. 各层如何调用 ICMP

#### 7.1 架构概览

```cpp
// 各层都持有 ICMPHandler 的引用，用于发送错误

class IPv4Layer {
    ICMPHandler* icmp_ = nullptr;  // 可选，用于发送错误
public:
    void set_icmp_handler(ICMPHandler* h) { icmp_ = h; }
};

class UDPLayer {
    ICMPHandler* icmp_ = nullptr;
public:
    void set_icmp_handler(ICMPHandler* h) { icmp_ = h; }
};
```

#### 7.2 IPv4 层调用 ICMP

```cpp
// src/net/ipv4.cpp

void IPv4Layer::on_receive(const uint8_t* data, size_t len) {
    auto pkt = IPv4Parser::parse(data, len);
    if (!pkt) return;

    // 检查目标地址
    if (pkt->dst_addr != _local_ip && pkt->dst_addr != 0xFFFFFFFF) {
        return;  // 不是发给我们的
    }

    // ══════════════════════════════════════════════════════════
    // 检查 TTL (如果我们作为路由器转发时)
    // ══════════════════════════════════════════════════════════
    // if (pkt->ttl == 0) {
    //     if (icmp_) {
    //         icmp_->send_time_exceeded(ICMPTimeExCode::TTLExceeded, *pkt);
    //     }
    //     return;
    // }

    // 查找上层协议处理器
    auto it = _handlers.find(pkt->protocol);
    if (it != _handlers.end()) {
        it->second->handle(*pkt);
    } else {
        // ══════════════════════════════════════════════════════════
        // 未知协议 → 发送 ICMP Protocol Unreachable
        // ══════════════════════════════════════════════════════════
        if (icmp_) {
            icmp_->send_dest_unreachable(ICMPUnreachCode::ProtocolUnreachable, *pkt);
        }
    }
}
```

#### 7.3 UDP 层调用 ICMP

```cpp
// src/transport/udp.cpp

void UDPLayer::on_receive(const IPv4Packet& pkt) {
    auto* udp_hdr = reinterpret_cast<const UDPHeader*>(pkt.payload);
    uint16_t dst_port = ntohs(udp_hdr->dst_port);

    // 查找绑定到该端口的 socket
    auto it = sockets_.find(dst_port);
    if (it != sockets_.end()) {
        // 找到了，交给 socket 处理
        it->second->on_receive(pkt);
    } else {
        // ══════════════════════════════════════════════════════════
        // 端口未监听 → 发送 ICMP Port Unreachable
        // ══════════════════════════════════════════════════════════
        if (icmp_) {
            icmp_->send_dest_unreachable(ICMPUnreachCode::PortUnreachable, pkt);
        }
    }
}
```

---

### 8. 集成到 main.cpp

```cpp
#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/common/ip_addr.hpp"

int main() {
    // 创建设备
    auto device = NetDevice::create();
    if (!device || device->open() < 0) {
        return EXIT_FAILURE;
    }

    // 创建 IPv4 层
    IPv4Layer ip_layer(*device);
    ip_layer.set_local_ip(ip_from_string("192.168.100.2"));

    // 创建 ICMP 处理器并注册
    ICMPHandler icmp_handler(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::ICMP),
                              &icmp_handler);

    // 让 IPv4 层可以发送 ICMP 错误
    ip_layer.set_icmp_handler(&icmp_handler);

    // 主循环
    uint8_t buf[2048];
    while (running) {
        ssize_t n = device->recv(buf, sizeof(buf), 1000);
        if (n > 0) {
            ip_layer.on_receive(buf, n);
        }
    }

    return 0;
}
```

---

### 9. 测试 ICMP

#### 9.1 测试 Echo (ping)

```bash
# 终端 1: 运行 NeuStack
sudo ./build/neustack

# 终端 2: 配置接口并 ping
sudo ifconfig utun4 192.168.100.1 192.168.100.2 up
ping 192.168.100.2
```

预期输出：

```
# NeuStack
ICMP Echo Request from 192.168.100.1, id=1234, seq=0
ICMP Echo Reply sent to 192.168.100.1

# ping
64 bytes from 192.168.100.2: icmp_seq=0 ttl=64 time=0.5 ms
```

#### 9.2 测试 Port Unreachable (需要实现 UDP)

```bash
# 发送 UDP 到未监听的端口
nc -u 192.168.100.2 12345
hello
```

预期：收到 "Connection refused" 或看到 NeuStack 发送 ICMP。

#### 9.3 测试 Protocol Unreachable

可以用原始套接字发送未知协议号的 IP 包来触发。

---

### 10. ICMP 实现注意事项

#### 10.1 不回复 ICMP 错误的情况

RFC 1122 规定，以下情况**不应**发送 ICMP 错误：

1. **ICMP 错误消息** - 收到 Type 3/11/12 等错误不回复，避免错误风暴
2. **广播/多播地址** - 目标是广播地址的包
3. **链路层广播** - 通过链路层广播发送的包
4. **分片中的非首片** - fragment_offset > 0 的分片
5. **源地址异常** - 源地址是 0.0.0.0、广播、多播

```cpp
bool should_send_icmp_error(const IPv4Packet& pkt) {
    // 广播地址
    if (pkt.dst_addr == 0xFFFFFFFF) return false;

    // 多播地址 (224.0.0.0 - 239.255.255.255)
    if ((pkt.dst_addr >> 24) >= 224 && (pkt.dst_addr >> 24) <= 239) return false;

    // 源地址是广播或全零
    if (pkt.src_addr == 0 || pkt.src_addr == 0xFFFFFFFF) return false;

    // 非首片
    if (pkt.fragment_offset > 0) return false;

    return true;
}
```

#### 10.2 ICMP 速率限制

为防止被用作放大攻击，应对 ICMP 错误消息进行速率限制：

```cpp
class ICMPRateLimiter {
    std::chrono::steady_clock::time_point last_error_time_;
    static constexpr auto MIN_INTERVAL = std::chrono::milliseconds(100);

public:
    bool allow() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_error_time_ >= MIN_INTERVAL) {
            last_error_time_ = now;
            return true;
        }
        return false;
    }
};
```

---

### 11. 检查清单

#### 核心功能

- [ ] 解析 ICMP Echo Request (Type 8)
- [ ] 发送 ICMP Echo Reply (Type 0)
- [ ] `ping` 命令能正常收到回复
- [ ] 校验和计算正确

#### 错误报告

- [ ] 实现 `send_dest_unreachable()` 方法
- [ ] 实现 `send_time_exceeded()` 方法
- [ ] IPv4 层能调用 ICMP 报告未知协议
- [ ] (可选) UDP 层能调用 ICMP 报告端口不可达

#### 安全检查

- [ ] 不对 ICMP 错误消息回复 ICMP 错误
- [ ] 不对广播/多播地址发送 ICMP 错误
- [ ] (可选) 实现速率限制

---

## 第二部分：ARP

### 12. ARP 概述

ARP (Address Resolution Protocol) 用于将 IP 地址解析为 MAC 地址。

**重要：macOS utun 是 L3 (网络层) 设备，不需要 ARP！**

ARP 只在以下情况需要：
- Linux 使用 TAP (L2) 设备
- 真实以太网环境

如果你只用 macOS utun 或 Linux TUN (L3)，可以跳过本节。

```
┌─────────────────────────────────────────┐
│              IPv4 层                     │
│        "我要发给 192.168.1.100"          │
└─────────────────┬───────────────────────┘
                  │ 查询 MAC 地址
                  ▼
┌─────────────────────────────────────────┐
│               ARP 层                     │
│  ┌───────────────────────────────────┐  │
│  │          ARP 缓存表                │  │
│  │  192.168.1.100 → aa:bb:cc:dd:ee:ff│  │
│  └───────────────────────────────────┘  │
└─────────────────┬───────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│              以太网层                    │
│     封装目标 MAC: aa:bb:cc:dd:ee:ff     │
└─────────────────────────────────────────┘
```

---

### 13. ARP 报文格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Hardware Type         |         Protocol Type         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  HW Addr Len  | Proto Addr Len|           Operation           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Sender Hardware Address                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Sender Protocol Address                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Target Hardware Address                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Target Protocol Address                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 值 | 说明 |
|------|-----|------|
| Hardware Type | 1 | 以太网 |
| Protocol Type | 0x0800 | IPv4 |
| HW Addr Len | 6 | MAC 地址长度 |
| Proto Addr Len | 4 | IPv4 地址长度 |
| Operation | 1 或 2 | 1=Request, 2=Reply |

---

### 14. 数据结构

#### 14.1 文件位置

```
include/neustack/net/arp.hpp
src/net/arp.cpp
```

#### 14.2 ARP 头部结构

```cpp
// ARP 操作码
enum class ARPOperation : uint16_t {
    Request = 1,
    Reply   = 2,
};

// ARP 报文结构 (以太网 + IPv4)
struct ARPPacket {
    uint16_t hw_type;        // Hardware Type (1 = Ethernet)
    uint16_t proto_type;     // Protocol Type (0x0800 = IPv4)
    uint8_t  hw_len;         // Hardware Address Length (6)
    uint8_t  proto_len;      // Protocol Address Length (4)
    uint16_t operation;      // Operation (1=Request, 2=Reply)
    uint8_t  sender_mac[6];  // Sender MAC
    uint8_t  sender_ip[4];   // Sender IP
    uint8_t  target_mac[6];  // Target MAC
    uint8_t  target_ip[4];   // Target IP
};

static_assert(sizeof(ARPPacket) == 28, "ARPPacket must be 28 bytes");
```

---

### 15. ARP 缓存

#### 15.1 缓存条目

```cpp
struct ARPEntry {
    uint8_t mac[6];
    std::chrono::steady_clock::time_point expire_time;
    bool is_static;  // 静态条目永不过期
};

class ARPCache {
public:
    // 查询 MAC 地址
    bool lookup(uint32_t ip, uint8_t* mac_out);

    // 更新缓存
    void update(uint32_t ip, const uint8_t* mac, bool is_static = false);

    // 删除条目
    void remove(uint32_t ip);

    // 清理过期条目
    void cleanup();

private:
    std::unordered_map<uint32_t, ARPEntry> cache_;
    std::mutex mutex_;

    static constexpr auto ENTRY_TIMEOUT = std::chrono::minutes(5);
};
```

#### 15.2 实现

```cpp
bool ARPCache::lookup(uint32_t ip, uint8_t* mac_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(ip);
    if (it == cache_.end()) {
        return false;
    }

    auto& entry = it->second;

    // 检查是否过期
    if (!entry.is_static &&
        std::chrono::steady_clock::now() > entry.expire_time) {
        cache_.erase(it);
        return false;
    }

    std::memcpy(mac_out, entry.mac, 6);
    return true;
}

void ARPCache::update(uint32_t ip, const uint8_t* mac, bool is_static) {
    std::lock_guard<std::mutex> lock(mutex_);

    ARPEntry entry;
    std::memcpy(entry.mac, mac, 6);
    entry.expire_time = std::chrono::steady_clock::now() + ENTRY_TIMEOUT;
    entry.is_static = is_static;

    cache_[ip] = entry;
}
```

---

### 16. ARP 处理器

#### 16.1 类设计

```cpp
class ARPHandler {
public:
    ARPHandler(NetDevice& device, uint32_t local_ip, const uint8_t* local_mac);

    // 处理收到的 ARP 报文
    void handle(const uint8_t* data, size_t len);

    // 解析 IP 地址 (可能发送 ARP 请求)
    bool resolve(uint32_t ip, uint8_t* mac_out);

    // 获取缓存
    ARPCache& cache() { return cache_; }

private:
    void handle_request(const ARPPacket* arp);
    void handle_reply(const ARPPacket* arp);
    void send_request(uint32_t target_ip);
    void send_reply(uint32_t target_ip, const uint8_t* target_mac);

    NetDevice& device_;
    uint32_t local_ip_;
    uint8_t local_mac_[6];
    ARPCache cache_;
};
```

#### 16.2 处理 ARP 请求

```cpp
void ARPHandler::handle_request(const ARPPacket* arp) {
    // 提取目标 IP
    uint32_t target_ip = ip_from_bytes(arp->target_ip);

    // 检查是否是询问我们的 IP
    if (target_ip != local_ip_) {
        return;  // 不是问我们的
    }

    // 更新发送者的缓存
    uint32_t sender_ip = ip_from_bytes(arp->sender_ip);
    cache_.update(sender_ip, arp->sender_mac);

    // 发送 ARP Reply
    send_reply(sender_ip, arp->sender_mac);

    std::printf("ARP: Who has %s? Tell %s -> Replied\n",
        ip_to_string(target_ip).c_str(),
        ip_to_string(sender_ip).c_str());
}

void ARPHandler::send_reply(uint32_t target_ip, const uint8_t* target_mac) {
    ARPPacket arp{};

    arp.hw_type = htons(1);       // Ethernet
    arp.proto_type = htons(0x0800); // IPv4
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.operation = htons(static_cast<uint16_t>(ARPOperation::Reply));

    // Sender = 我们
    std::memcpy(arp.sender_mac, local_mac_, 6);
    arp.sender_ip[0] = (local_ip_ >> 24) & 0xFF;
    arp.sender_ip[1] = (local_ip_ >> 16) & 0xFF;
    arp.sender_ip[2] = (local_ip_ >> 8) & 0xFF;
    arp.sender_ip[3] = local_ip_ & 0xFF;

    // Target = 请求者
    std::memcpy(arp.target_mac, target_mac, 6);
    arp.target_ip[0] = (target_ip >> 24) & 0xFF;
    arp.target_ip[1] = (target_ip >> 16) & 0xFF;
    arp.target_ip[2] = (target_ip >> 8) & 0xFF;
    arp.target_ip[3] = target_ip & 0xFF;

    // 发送 (需要封装以太网帧)
    send_ethernet_frame(target_mac, ETHERTYPE_ARP, &arp, sizeof(arp));
}
```

---

### 17. 以太网帧封装

如果使用 TAP 设备，需要处理以太网帧：

```cpp
// 以太网帧头部
struct EthernetHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;  // 0x0800=IPv4, 0x0806=ARP
};

constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
constexpr uint16_t ETHERTYPE_ARP  = 0x0806;

void send_ethernet_frame(const uint8_t* dst_mac, uint16_t ethertype,
                          const void* payload, size_t len) {
    uint8_t frame[1514];  // 最大以太网帧

    auto* hdr = reinterpret_cast<EthernetHeader*>(frame);
    std::memcpy(hdr->dst_mac, dst_mac, 6);
    std::memcpy(hdr->src_mac, local_mac_, 6);
    hdr->ethertype = htons(ethertype);

    std::memcpy(frame + sizeof(EthernetHeader), payload, len);

    device_.send(frame, sizeof(EthernetHeader) + len);
}
```

---

### 18. 何时需要 ARP

| 设备类型 | 层级 | 需要 ARP | 说明 |
|----------|------|----------|------|
| macOS utun | L3 | ❌ | 点对点，无 MAC 地址 |
| Linux TUN | L3 | ❌ | 点对点，无 MAC 地址 |
| Linux TAP | L2 | ✅ | 模拟以太网 |
| Windows Wintun | L3 | ❌ | 点对点 |

**你的 macOS utun 实现不需要 ARP！**

如果以后要支持 Linux TAP，可以：
1. 在 HAL 层添加 `is_l2()` 方法
2. 根据设备类型决定是否启用 ARP/以太网层

---

### 19. 检查清单

#### ICMP

- [ ] 能解析 ICMP Echo Request
- [ ] 能发送 ICMP Echo Reply
- [ ] `ping` 命令能收到回复
- [ ] 校验和计算正确
- [ ] 能处理不同大小的 ping 数据
- [ ] 实现 Destination Unreachable 发送
- [ ] 实现 Time Exceeded 发送
- [ ] 不对 ICMP 错误回复 ICMP 错误

#### ARP (如果使用 TAP)

- [ ] 能解析 ARP Request
- [ ] 能发送 ARP Reply
- [ ] ARP 缓存能正确存储和查询
- [ ] 缓存条目能正确过期
- [ ] 能主动发送 ARP Request

---

### 20. 常见问题

#### Q1: ping 收到回复但时间很长？

检查是否在收到请求后立即发送回复，而不是等待下一次循环。

#### Q2: ping 显示 "DUP!" (重复)?

可能发送了多个回复。检查是否重复处理了同一个请求。

#### Q3: ICMP 校验和错误？

ICMP 校验和覆盖整个 ICMP 报文 (头部 + 数据)，不是只有头部！

#### Q4: ICMP 错误消息被对方忽略？

确保错误消息中包含了原始 IP 头部和前 8 字节数据，否则对方无法识别是哪个包出了问题。

#### Q5: ARP 请求收不到回复？

检查：
1. 目标 MAC 是否设置为广播地址 (ff:ff:ff:ff:ff:ff)
2. 是否正确封装了以太网帧
3. TAP 设备是否正确配置

---

### 21. 参考资料

- [RFC 792: Internet Control Message Protocol](https://datatracker.ietf.org/doc/html/rfc792)
- [RFC 826: Address Resolution Protocol](https://datatracker.ietf.org/doc/html/rfc826)
- [RFC 1122: Requirements for Internet Hosts](https://datatracker.ietf.org/doc/html/rfc1122)
- [RFC 1812: Requirements for IP Version 4 Routers](https://datatracker.ietf.org/doc/html/rfc1812) (ICMP 错误生成规则)
