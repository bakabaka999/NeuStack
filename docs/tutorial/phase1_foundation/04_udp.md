# UDP 层实现教程

## 1. UDP 概述

UDP (User Datagram Protocol) 是最简单的传输层协议：

- **无连接** - 不需要建立连接，直接发送
- **不可靠** - 不保证送达，不重传
- **无序** - 不保证顺序
- **轻量** - 头部只有 8 字节

```
┌─────────────────────────────────────────────────────────────────┐
│                         应用层                                   │
│         ┌─────────┐              ┌─────────┐                    │
│         │ DNS客户端│              │ 游戏服务 │                    │
│         └────┬────┘              └────┬────┘                    │
└──────────────┼───────────────────────┼──────────────────────────┘
               │                       │
               ▼                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                         UDP 层                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    Socket 管理                           │    │
│  │   Port 53 → DNS Socket                                  │    │
│  │   Port 8080 → Game Socket                               │    │
│  └─────────────────────────────────────────────────────────┘    │
│                            │                                     │
│                            ▼                                     │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              UDP 报文解析 / 构建                          │    │
│  └─────────────────────────────────────────────────────────┘    │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                         IPv4 层                                  │
│                      Protocol = 17                               │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. UDP 报文格式

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            Length             |           Checksum            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                             Data                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 大小 | 说明 |
|------|------|------|
| Source Port | 2 字节 | 源端口 (0-65535) |
| Destination Port | 2 字节 | 目标端口 |
| Length | 2 字节 | UDP 头部 + 数据的总长度 (最小 8) |
| Checksum | 2 字节 | 校验和 (含伪头部) |
| Data | 可变 | 应用数据 |

**头部只有 8 字节！** 对比 TCP 的 20+ 字节，UDP 非常轻量。

---

## 3. UDP 校验和

UDP 校验和计算需要包含 **伪头部 (Pseudo Header)**：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Source IP                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Destination IP                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      Zero     |   Protocol    |          UDP Length           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

校验和计算步骤：
1. 构造伪头部 (12 字节)
2. 拼接 UDP 头部 + 数据
3. 对整体计算 Internet Checksum

**注意**：UDP 校验和在 IPv4 中是可选的（设为 0 表示不校验），但建议始终计算。

---

## 4. 数据结构设计

### 4.1 文件位置

```
include/neustack/transport/udp.hpp
src/transport/udp.cpp
```

### 4.2 UDP 头部结构

```cpp
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
```

### 4.3 UDP 数据报结构

```cpp
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
    const uint8_t* data;
    size_t data_length;
};
```

---

## 5. UDPLayer 设计

### 5.1 类接口

```cpp
// ============================================================================
// UDP Socket 回调
// ============================================================================

// 收到数据时的回调函数
// 参数: 源IP, 源端口, 数据指针, 数据长度
using UDPReceiveCallback = std::function<void(uint32_t src_ip, uint16_t src_port,
                                               const uint8_t* data, size_t len)>;

// ============================================================================
// UDPLayer - UDP 层
// ============================================================================

class UDPLayer : public IProtocolHandler {
public:
    explicit UDPLayer(IPv4Layer& ip_layer);

    // 实现 IProtocolHandler 接口
    void handle(const IPv4Packet& pkt) override;

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
                   const uint8_t* data, size_t len);

private:
    // 解析 UDP 数据报
    std::optional<UDPDatagram> parse(const IPv4Packet& pkt);

    // 验证校验和
    bool verify_checksum(const IPv4Packet& pkt);

    // 计算校验和
    uint16_t compute_udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                   const uint8_t* udp_packet, size_t udp_len);

    // 分配临时端口
    uint16_t allocate_ephemeral_port();

    IPv4Layer& _ip_layer;
    std::unordered_map<uint16_t, UDPReceiveCallback> _sockets;
    uint16_t _next_ephemeral_port = 49152;  // 临时端口范围: 49152-65535
};

#endif // NEUSTACK_TRANSPORT_UDP_HPP
```

---

## 6. 实现

### 6.1 构造函数和注册

```cpp
// src/transport/udp.cpp

#include "neustack/transport/udp.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/common/checksum.hpp"
#include "neustack/common/ip_addr.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>

UDPLayer::UDPLayer(IPv4Layer& ip_layer)
    : _ip_layer(ip_layer)
    , _next_ephemeral_port(49152)
{}
```

### 6.2 接收处理

```cpp
void UDPLayer::handle(const IPv4Packet& pkt) {
    auto datagram = parse(pkt);
    if (!datagram) {
        return;
    }

    std::printf("UDP: %s:%u -> %s:%u, len=%zu\n",
                ip_to_string(datagram->src_addr).c_str(),
                datagram->src_port,
                ip_to_string(datagram->dst_addr).c_str(),
                datagram->dst_port,
                datagram->data_length);

    // 查找绑定的 socket
    auto it = _sockets.find(datagram->dst_port);
    if (it != _sockets.end()) {
        // 调用回调
        it->second(datagram->src_addr, datagram->src_port,
                   datagram->data, datagram->data_length);
    } else {
        // 端口未绑定，发送 ICMP Port Unreachable
        auto icmp_it = _ip_layer._handlers.find(static_cast<uint8_t>(IPProtocol::ICMP));
        if (icmp_it != _ip_layer._handlers.end()) {
            auto* icmp = dynamic_cast<ICMPHandler*>(icmp_it->second);
            if (icmp) {
                icmp->send_dest_unreachable(ICMPUnreachCode::PortUnreachable, pkt);
            }
        }
    }
}

std::optional<UDPDatagram> UDPLayer::parse(const IPv4Packet& pkt) {
    // 最小长度检查
    if (pkt.payload_length < sizeof(UDPHeader)) {
        return std::nullopt;
    }

    auto* hdr = reinterpret_cast<const UDPHeader*>(pkt.payload);

    // 长度检查
    uint16_t udp_len = ntohs(hdr->length);
    if (udp_len < 8 || udp_len > pkt.payload_length) {
        return std::nullopt;
    }

    // 校验和验证 (如果不为 0)
    if (hdr->checksum != 0 && !verify_checksum(pkt)) {
        std::printf("UDP: checksum error\n");
        return std::nullopt;
    }

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
```

### 6.3 校验和

```cpp
bool UDPLayer::verify_checksum(const IPv4Packet& pkt) {
    uint16_t checksum = compute_udp_checksum(pkt.src_addr, pkt.dst_addr,
                                              pkt.payload, pkt.payload_length);
    return checksum == 0;
}

uint16_t UDPLayer::compute_udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                         const uint8_t* udp_packet, size_t udp_len) {
    // 构造伪头部 + UDP 数据
    std::vector<uint8_t> buffer(sizeof(UDPPseudoHeader) + udp_len);

    // 填充伪头部
    auto* pseudo = reinterpret_cast<UDPPseudoHeader*>(buffer.data());
    pseudo->src_addr = htonl(src_ip);
    pseudo->dst_addr = htonl(dst_ip);
    pseudo->zero = 0;
    pseudo->protocol = 17;  // UDP
    pseudo->udp_length = htons(static_cast<uint16_t>(udp_len));

    // 复制 UDP 数据
    std::memcpy(buffer.data() + sizeof(UDPPseudoHeader), udp_packet, udp_len);

    // 如果长度为奇数，补 0 (checksum 计算需要)
    if (buffer.size() % 2 != 0) {
        buffer.push_back(0);
    }

    return compute_checksum(buffer.data(), buffer.size());
}
```

### 6.4 发送

```cpp
ssize_t UDPLayer::sendto(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                          const uint8_t* data, size_t len) {
    // 检查源端口是否已绑定
    if (_sockets.find(src_port) == _sockets.end()) {
        std::printf("UDP: source port %u not bound\n", src_port);
        return -1;
    }

    // 构造 UDP 报文
    size_t udp_len = sizeof(UDPHeader) + len;
    std::vector<uint8_t> buffer(udp_len);

    auto* hdr = reinterpret_cast<UDPHeader*>(buffer.data());
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(static_cast<uint16_t>(udp_len));
    hdr->checksum = 0;  // 先设为 0

    // 复制数据
    if (data && len > 0) {
        std::memcpy(buffer.data() + sizeof(UDPHeader), data, len);
    }

    // 计算校验和
    hdr->checksum = compute_udp_checksum(_ip_layer.local_ip(), dst_ip,
                                          buffer.data(), udp_len);
    // 如果校验和结果为 0，设为 0xFFFF (UDP 规定)
    if (hdr->checksum == 0) {
        hdr->checksum = 0xFFFF;
    }

    // 通过 IP 层发送
    return _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::UDP),
                          buffer.data(), udp_len);
}
```

### 6.5 端口绑定

```cpp
uint16_t UDPLayer::bind(uint16_t port, UDPReceiveCallback callback) {
    if (!callback) {
        return 0;
    }

    // 如果端口为 0，自动分配
    if (port == 0) {
        port = allocate_ephemeral_port();
        if (port == 0) {
            return 0;  // 无可用端口
        }
    }

    // 检查端口是否已被占用
    if (_sockets.find(port) != _sockets.end()) {
        std::printf("UDP: port %u already bound\n", port);
        return 0;
    }

    _sockets[port] = std::move(callback);
    std::printf("UDP: bound to port %u\n", port);
    return port;
}

void UDPLayer::unbind(uint16_t port) {
    auto it = _sockets.find(port);
    if (it != _sockets.end()) {
        _sockets.erase(it);
        std::printf("UDP: unbound port %u\n", port);
    }
}

uint16_t UDPLayer::allocate_ephemeral_port() {
    // 临时端口范围: 49152-65535
    uint16_t start = _next_ephemeral_port;

    do {
        if (_sockets.find(_next_ephemeral_port) == _sockets.end()) {
            uint16_t port = _next_ephemeral_port;
            _next_ephemeral_port++;
            if (_next_ephemeral_port == 0) {
                _next_ephemeral_port = 49152;
            }
            return port;
        }

        _next_ephemeral_port++;
        if (_next_ephemeral_port == 0) {
            _next_ephemeral_port = 49152;
        }
    } while (_next_ephemeral_port != start);

    return 0;  // 所有端口都被占用
}
```

---

## 7. 访问 IPv4 层的 handlers

为了让 UDP 层能发送 ICMP Port Unreachable，需要访问 IPv4 层的 `_handlers`。有两种方案：

### 方案 A：在 IPv4Layer 添加 getter（推荐）

```cpp
// include/neustack/net/ipv4.hpp
class IPv4Layer {
public:
    // 获取 handler (供其他层使用)
    IProtocolHandler* get_handler(uint8_t protocol) const {
        auto it = _handlers.find(protocol);
        return it != _handlers.end() ? it->second : nullptr;
    }
    // ...
};
```

然后 UDP 层这样使用：

```cpp
auto* handler = _ip_layer.get_handler(static_cast<uint8_t>(IPProtocol::ICMP));
if (handler) {
    auto* icmp = dynamic_cast<ICMPHandler*>(handler);
    if (icmp) {
        icmp->send_dest_unreachable(ICMPUnreachCode::PortUnreachable, pkt);
    }
}
```

### 方案 B：UDP 层单独存 ICMP 指针

```cpp
class UDPLayer {
public:
    void set_icmp_handler(ICMPHandler* icmp) { _icmp = icmp; }
private:
    ICMPHandler* _icmp = nullptr;
};
```

---

## 8. 集成到 main.cpp

```cpp
#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/common/ip_addr.hpp"

int main() {
    // ... 设备创建代码 ...

    // 创建协议栈
    IPv4Layer ip_layer(*device);
    ip_layer.set_local_ip(ip_from_string("192.168.100.2"));

    ICMPHandler icmp_handler(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &icmp_handler);

    UDPLayer udp_layer(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::UDP), &udp_layer);

    // 绑定一个 UDP 端口用于测试
    uint16_t echo_port = udp_layer.bind(7777, [&](uint32_t src_ip, uint16_t src_port,
                                                    const uint8_t* data, size_t len) {
        std::printf("Received %zu bytes from %s:%u\n",
                    len, ip_to_string(src_ip).c_str(), src_port);

        // Echo 回去
        udp_layer.sendto(src_ip, src_port, 7777, data, len);
    });

    std::printf("UDP Echo server listening on port %u\n", echo_port);

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

## 9. 测试

### 9.1 启动服务

```bash
# 终端 1: 运行 NeuStack
sudo ./build/neustack

# 终端 2: 配置接口
sudo ifconfig utunX 192.168.100.1 192.168.100.2 up
```

### 9.2 测试 Echo

```bash
# 终端 2: 使用 nc 发送 UDP
echo "Hello NeuStack" | nc -u 192.168.100.2 7777
```

预期：
- NeuStack 打印收到的数据
- nc 收到 echo 回来的数据

### 9.3 测试 Port Unreachable

```bash
# 发送到未绑定的端口
echo "test" | nc -u 192.168.100.2 9999
```

预期：
- NeuStack 发送 ICMP Port Unreachable
- nc 显示 "Connection refused" 或类似错误

---

## 10. 注意事项

### 10.1 校验和为 0 的特殊处理

UDP 规定：
- 发送时，如果计算出的校验和恰好为 0，必须设为 `0xFFFF`
- 接收时，校验和为 0 表示发送方未计算校验和，应跳过验证

```cpp
// 发送时
if (hdr->checksum == 0) {
    hdr->checksum = 0xFFFF;
}

// 接收时
if (hdr->checksum != 0 && !verify_checksum(pkt)) {
    return std::nullopt;  // 校验失败
}
```

### 10.2 最大数据长度

```cpp
// UDP 最大长度 = 65535 - 8 (UDP头) = 65527 字节
// 但受限于 IP MTU，实际单包最大约 1472 字节 (1500 - 20 IP - 8 UDP)
constexpr size_t UDP_MAX_PAYLOAD = 65527;
constexpr size_t UDP_MTU_PAYLOAD = 1472;  // 不分片时的实际限制
```

### 10.3 端口范围

| 范围 | 说明 |
|------|------|
| 0-1023 | 知名端口 (需要 root) |
| 1024-49151 | 注册端口 |
| 49152-65535 | 临时/私有端口 |

---

## 11. 更新 CMakeLists.txt

```cmake
# Transport layer sources
set(TRANSPORT_SOURCES
    src/transport/udp.cpp
    # src/transport/tcp.cpp
)
```

---

## 12. 检查清单

### 核心功能

- [ ] UDP 头部解析正确
- [ ] 校验和验证正确 (含伪头部)
- [ ] 校验和计算正确
- [ ] 端口绑定/解绑功能
- [ ] 数据收发功能
- [ ] 临时端口分配

### 错误处理

- [ ] 端口未绑定时发送 ICMP Port Unreachable
- [ ] 校验和错误时丢弃
- [ ] 长度不合法时丢弃

### 测试

- [ ] nc -u 能发送数据到 NeuStack
- [ ] NeuStack 能 echo 数据回去
- [ ] 未绑定端口返回 ICMP 错误

---

## 13. 常见问题

### Q1: nc 发送后没反应？

检查：
1. 接口是否配置正确
2. IP 地址是否匹配
3. 端口是否已绑定
4. 是否注册了 UDP handler 到 IPv4 层

### Q2: 校验和一直失败？

检查：
1. 伪头部的 IP 地址字节序是否正确
2. UDP 长度是否正确
3. 是否包含了完整的 UDP 数据

### Q3: 收到数据但 echo 不回去？

检查：
1. sendto 的返回值
2. 源端口是否已绑定
3. 目标 IP/端口 是否正确

---

## 14. 参考资料

- [RFC 768: User Datagram Protocol](https://datatracker.ietf.org/doc/html/rfc768)
- [RFC 1122: Requirements for Internet Hosts](https://datatracker.ietf.org/doc/html/rfc1122)
