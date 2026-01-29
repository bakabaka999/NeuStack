# 09: TCPLayer 封装与协议栈整合

前面几章我们实现了 TCP 的核心功能。本章将：

1. 将 `TCPConnectionManager` 封装为 `TCPLayer`（实现 `IProtocolHandler` 接口）
2. 提供简洁的应用层 API
3. 完成与 IPv4Layer 的整合

## 1. 回顾现有架构

### 1.1 协议栈分层

```
┌─────────────────────────────────────────────────────┐
│                    应用层                            │
│              (Echo Server, HTTP, etc.)              │
├─────────────────────────────────────────────────────┤
│         传输层: TCPLayer / UDPLayer                  │
│         实现 IProtocolHandler 接口                   │
├─────────────────────────────────────────────────────┤
│         网络层: IPv4Layer                            │
│         路由、分发到上层协议处理器                      │
├─────────────────────────────────────────────────────┤
│         链路层: NetDevice (HAL)                      │
│         TUN/TAP 设备抽象                             │
└─────────────────────────────────────────────────────┘
```

### 1.2 现有接口

**IProtocolHandler** (`protocol_handler.hpp`):
```cpp
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;
    virtual void handle(const IPv4Packet& pkt) = 0;
};
```

**UDPLayer** 的实现方式（我们要参考）:
```cpp
class UDPLayer : public IProtocolHandler {
public:
    explicit UDPLayer(IPv4Layer& ip_layer);
    void handle(const IPv4Packet& pkt) override;

    // Socket API
    uint16_t bind(uint16_t port, UDPReceiveCallback callback);
    void unbind(uint16_t port);
    ssize_t sendto(...);

private:
    IPv4Layer& _ip_layer;
    std::unordered_map<uint16_t, UDPReceiveCallback> _sockets;
};
```

### 1.3 当前 main.cpp 中的临时实现

我们在 `main.cpp` 中有一个临时的 `TCPHandler` 类：

```cpp
class TCPHandler : public IProtocolHandler {
public:
    TCPHandler(IPv4Layer& ip_layer, uint32_t local_ip)
        : _ip_layer(ip_layer), _tcp_mgr(local_ip) {
        _tcp_mgr.set_send_callback([this](uint32_t dst_ip, const uint8_t* data, size_t len) {
            _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::TCP), data, len);
        });
    }

    void handle(const IPv4Packet& pkt) override { ... }
    void on_timer() { _tcp_mgr.on_timer(); }
    TCPConnectionManager& manager() { return _tcp_mgr; }

private:
    IPv4Layer& _ip_layer;
    TCPConnectionManager _tcp_mgr;
};
```

现在我们要把它正式化为 `TCPLayer`。

## 2. TCPLayer 设计

### 2.1 设计目标

1. **实现 IProtocolHandler**：可以注册到 IPv4Layer
2. **封装 TCPConnectionManager**：隐藏内部实现
3. **提供简洁 API**：类似 BSD Socket 的接口
4. **支持定时器**：需要外部周期性调用

### 2.2 头文件设计

创建 `include/neustack/transport/tcp_layer.hpp`：

```cpp
#ifndef NEUSTACK_TRANSPORT_TCP_LAYER_HPP
#define NEUSTACK_TRANSPORT_TCP_LAYER_HPP

#include "neustack/net/ipv4.hpp"
#include "neustack/net/protocol_handler.hpp"
#include "neustack/transport/tcp_connection.hpp"
#include "neustack/transport/tcp_segment.hpp"

#include <memory>

namespace neustack {

// ============================================================================
// TCP 监听器回调
// ============================================================================

// 新连接回调：返回该连接的数据接收和关闭回调
struct TCPCallbacks {
    TCPReceiveCallback on_receive;
    TCPCloseCallback on_close;
};

// 接受新连接时的回调
// 参数: tcb - 新连接
// 返回: 该连接的回调函数
using TCPAcceptCallback = std::function<TCPCallbacks(TCB* tcb)>;

// ============================================================================
// TCPLayer - TCP 传输层
// ============================================================================

class TCPLayer : public IProtocolHandler {
public:
    /**
     * @brief 构造 TCP 层
     * @param ip_layer IPv4 层引用
     * @param local_ip 本机 IP 地址
     */
    TCPLayer(IPv4Layer& ip_layer, uint32_t local_ip);

    // ─── IProtocolHandler 接口 ───

    /**
     * @brief 处理收到的 IPv4 包
     */
    void handle(const IPv4Packet& pkt) override;

    // ─── 定时器 ───

    /**
     * @brief 定时器回调，需要周期性调用（建议 100ms）
     */
    void on_timer();

    // ─── 服务器 API ───

    /**
     * @brief 监听端口
     * @param port 本地端口
     * @param on_accept 新连接回调
     * @return 0 成功，-1 失败
     */
    int listen(uint16_t port, TCPAcceptCallback on_accept);

    /**
     * @brief 停止监听
     * @param port 端口号
     */
    void unlisten(uint16_t port);

    // ─── 客户端 API ───

    /**
     * @brief 连接远程主机
     * @param remote_ip 远程 IP
     * @param remote_port 远程端口
     * @param local_port 本地端口（0 = 自动分配）
     * @param on_connect 连接完成回调
     * @param on_receive 数据接收回调
     * @param on_close 连接关闭回调
     * @return 0 成功（异步），-1 失败
     */
    int connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                TCPConnectCallback on_connect,
                TCPReceiveCallback on_receive,
                TCPCloseCallback on_close);

    // ─── 数据传输 API ───

    /**
     * @brief 发送数据
     * @param tcb 连接
     * @param data 数据
     * @param len 长度
     * @return 发送的字节数，-1 失败
     */
    ssize_t send(TCB* tcb, const uint8_t* data, size_t len);

    /**
     * @brief 关闭连接
     * @param tcb 连接
     * @return 0 成功，-1 失败
     */
    int close(TCB* tcb);

private:
    IPv4Layer& _ip_layer;
    uint32_t _local_ip;
    TCPConnectionManager _tcp_mgr;

    // 监听回调表
    std::unordered_map<uint16_t, TCPAcceptCallback> _accept_callbacks;

    // 临时端口分配
    uint16_t _next_ephemeral_port = 49152;
    uint16_t allocate_ephemeral_port();
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_LAYER_HPP
```

### 2.3 实现

创建 `src/transport/tcp_layer.cpp`：

```cpp
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"

namespace neustack {

TCPLayer::TCPLayer(IPv4Layer& ip_layer, uint32_t local_ip)
    : _ip_layer(ip_layer)
    , _local_ip(local_ip)
    , _tcp_mgr(local_ip)
{
    // 设置发送回调：TCP -> IP 层
    _tcp_mgr.set_send_callback([this](uint32_t dst_ip, const uint8_t* data, size_t len) {
        _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::TCP), data, len);
    });
}

void TCPLayer::handle(const IPv4Packet& pkt) {
    auto seg = TCPParser::parse(pkt);
    if (seg) {
        _tcp_mgr.on_segment_received(*seg);
    } else {
        LOG_WARN(TCP, "Failed to parse TCP segment");
    }
}

void TCPLayer::on_timer() {
    _tcp_mgr.on_timer();
}

int TCPLayer::listen(uint16_t port, TCPAcceptCallback on_accept) {
    // 保存 accept 回调
    _accept_callbacks[port] = std::move(on_accept);

    // 调用底层 listen，设置 on_connect 回调
    int result = _tcp_mgr.listen(port, [this, port](TCB* tcb, int error) {
        if (error != 0) {
            LOG_WARN(TCP, "Connection failed on port %u: %d", port, error);
            return;
        }

        // 查找 accept 回调
        auto it = _accept_callbacks.find(port);
        if (it == _accept_callbacks.end()) {
            LOG_WARN(TCP, "No accept callback for port %u", port);
            return;
        }

        // 调用 accept 回调，获取该连接的回调
        TCPCallbacks callbacks = it->second(tcb);

        // 设置连接的回调
        tcb->on_receive = std::move(callbacks.on_receive);
        tcb->on_close = std::move(callbacks.on_close);

        LOG_INFO(TCP, "Accepted connection from %s:%u on port %u",
                 ip_to_string(tcb->t_tuple.remote_ip).c_str(),
                 tcb->t_tuple.remote_port, port);
    });

    if (result < 0) {
        _accept_callbacks.erase(port);
    }

    return result;
}

void TCPLayer::unlisten(uint16_t port) {
    _accept_callbacks.erase(port);
    // TODO: 关闭监听 TCB
}

int TCPLayer::connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                      TCPConnectCallback on_connect,
                      TCPReceiveCallback on_receive,
                      TCPCloseCallback on_close)
{
    // 自动分配本地端口
    if (local_port == 0) {
        local_port = allocate_ephemeral_port();
    }

    // 创建包装的 on_connect 回调
    auto wrapped_connect = [on_connect, on_receive, on_close](TCB* tcb, int error) {
        if (error == 0) {
            // 连接成功，设置回调
            tcb->on_receive = on_receive;
            tcb->on_close = on_close;
        }
        // 通知应用层
        if (on_connect) {
            on_connect(tcb, error);
        }
    };

    return _tcp_mgr.connect(remote_ip, remote_port, local_port, wrapped_connect);
}

ssize_t TCPLayer::send(TCB* tcb, const uint8_t* data, size_t len) {
    return _tcp_mgr.send(tcb, data, len);
}

int TCPLayer::close(TCB* tcb) {
    return _tcp_mgr.close(tcb);
}

uint16_t TCPLayer::allocate_ephemeral_port() {
    // 简单的端口分配：从 49152 开始递增
    // 实际应该检查端口是否已被使用
    uint16_t port = _next_ephemeral_port++;
    if (_next_ephemeral_port > 65535) {
        _next_ephemeral_port = 49152;
    }
    return port;
}

} // namespace neustack
```

## 3. 更新 main.cpp

使用新的 `TCPLayer`：

```cpp
#include "neustack/hal/device.hpp"
#include "neustack/net/ipv4.hpp"
#include "neustack/net/icmp.hpp"
#include "neustack/transport/udp.hpp"
#include "neustack/transport/tcp_layer.hpp"
#include "neustack/common/ip_addr.hpp"
#include "neustack/common/log.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

using namespace neustack;

static volatile bool running = true;

void signal_handler(int) {
    running = false;
}

int main(int argc, char* argv[]) {
    // ... 日志配置（现有代码）...

    LOG_INFO(APP, "NeuStack v0.1.0 starting");

    // 注册信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 创建设备
    auto device = NetDevice::create();
    if (!device || device->open() < 0) {
        LOG_FATAL(HAL, "Failed to open device");
        return EXIT_FAILURE;
    }

    LOG_INFO(HAL, "Device %s opened (fd=%d)", device->get_name().c_str(), device->get_fd());

    // 创建协议栈
    uint32_t local_ip = ip_from_string("192.168.100.2");

    // 网络层
    IPv4Layer ip_layer(*device);
    ip_layer.set_local_ip(local_ip);

    // ICMP
    ICMPHandler icmp_handler(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::ICMP), &icmp_handler);

    // UDP
    UDPLayer udp_layer(ip_layer);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::UDP), &udp_layer);

    // TCP
    TCPLayer tcp_layer(ip_layer, local_ip);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &tcp_layer);

    // ─── 配置服务 ───

    // UDP Echo (端口 7)
    udp_layer.bind(7, [&udp_layer](uint32_t src_ip, uint16_t src_port,
                                    const uint8_t* data, size_t len) {
        LOG_INFO(UDP, "Echo: %zu bytes from %s:%u",
                 len, ip_to_string(src_ip).c_str(), src_port);
        udp_layer.sendto(src_ip, src_port, 7, data, len);
    });

    // TCP Echo (端口 7)
    tcp_layer.listen(7, [&tcp_layer](TCB* tcb) -> TCPCallbacks {
        LOG_INFO(TCP, "New connection from %s:%u",
                 ip_to_string(tcb->t_tuple.remote_ip).c_str(),
                 tcb->t_tuple.remote_port);

        return TCPCallbacks{
            // on_receive: echo 数据
            [&tcp_layer](TCB* tcb, const uint8_t* data, size_t len) {
                LOG_INFO(TCP, "Echo: %zu bytes", len);
                tcp_layer.send(tcb, data, len);
            },
            // on_close: 关闭连接
            [&tcp_layer](TCB* tcb) {
                LOG_INFO(TCP, "Peer closed");
                tcp_layer.close(tcb);
            }
        };
    });

    LOG_INFO(APP, "Local IP: %s", ip_to_string(local_ip).c_str());
    LOG_INFO(APP, "Services: ICMP ping, UDP echo :7, TCP echo :7");

    // 配置提示
    std::printf("\nConfigure interface:\n");
    std::printf("  sudo ifconfig %s 192.168.100.1 192.168.100.2 up\n\n",
                device->get_name().c_str());

    // 主循环
    uint8_t buf[2048];
    auto last_timer = std::chrono::steady_clock::now();
    constexpr auto TIMER_INTERVAL = std::chrono::milliseconds(100);

    while (running) {
        ssize_t n = device->recv(buf, sizeof(buf), 100);

        if (n > 0) {
            auto pkt = IPv4Parser::parse(buf, n);
            if (pkt) {
                ip_layer.on_receive(buf, n);
            }
        } else if (n < 0) {
            LOG_ERROR(HAL, "recv error: %s", std::strerror(errno));
            break;
        }

        // 定时器
        auto now = std::chrono::steady_clock::now();
        if (now - last_timer >= TIMER_INTERVAL) {
            tcp_layer.on_timer();
            last_timer = now;
        }
    }

    LOG_INFO(APP, "Shutting down");
    device->close();

    return EXIT_SUCCESS;
}
```

## 4. 更新 CMakeLists.txt

添加新文件：

```cmake
set(NEUSTACK_SOURCES
    src/hal/device.cpp
    src/net/ipv4.cpp
    src/net/icmp.cpp
    src/transport/udp.cpp
    src/transport/tcp_segment.cpp
    src/transport/tcp_builder.cpp
    src/transport/tcp_connection.cpp
    src/transport/tcp_layer.cpp        # 新增
    src/common/log.cpp
    src/common/ip_addr.cpp
    src/common/checksum.cpp
)
```

## 5. API 对比

### 5.1 与 BSD Socket 对比

| BSD Socket | NeuStack TCPLayer | 说明 |
|------------|-------------------|------|
| `socket()` | (自动) | 创建时自动处理 |
| `bind()` | (在 listen/connect 中) | 集成到其他操作 |
| `listen()` | `listen(port, callback)` | 异步回调模式 |
| `accept()` | (回调中处理) | 通过 accept 回调获取新连接 |
| `connect()` | `connect(...)` | 异步，结果通过回调 |
| `send()` | `send(tcb, data, len)` | 需要 TCB 指针 |
| `recv()` | (回调) | 通过 on_receive 回调 |
| `close()` | `close(tcb)` | 主动关闭 |

### 5.2 与 UDPLayer 对比

| 功能 | UDPLayer | TCPLayer |
|------|----------|----------|
| 绑定 | `bind(port, callback)` | `listen(port, callback)` |
| 发送 | `sendto(ip, port, ...)` | `send(tcb, data, len)` |
| 状态 | 无状态 | 有状态（TCB） |
| 定时器 | 不需要 | 需要 `on_timer()` |

## 6. 使用示例

### 6.1 TCP 服务器

```cpp
tcp_layer.listen(8080, [&](TCB* tcb) -> TCPCallbacks {
    // 新连接建立
    LOG_INFO(APP, "Client connected");

    return TCPCallbacks{
        // 收到数据
        [&](TCB* tcb, const uint8_t* data, size_t len) {
            // 处理请求
            std::string request(reinterpret_cast<const char*>(data), len);
            LOG_INFO(APP, "Request: %s", request.c_str());

            // 发送响应
            std::string response = "HTTP/1.0 200 OK\r\n\r\nHello!";
            tcp_layer.send(tcb, reinterpret_cast<const uint8_t*>(response.data()),
                          response.size());
            tcp_layer.close(tcb);
        },
        // 连接关闭
        [](TCB* tcb) {
            LOG_INFO(APP, "Client disconnected");
        }
    };
});
```

### 6.2 TCP 客户端

```cpp
tcp_layer.connect(
    ip_from_string("192.168.100.1"), 80, 0,  // 远程地址，本地端口自动分配
    // on_connect
    [&](TCB* tcb, int error) {
        if (error == 0) {
            LOG_INFO(APP, "Connected!");
            std::string request = "GET / HTTP/1.0\r\n\r\n";
            tcp_layer.send(tcb, reinterpret_cast<const uint8_t*>(request.data()),
                          request.size());
        } else {
            LOG_ERROR(APP, "Connect failed: %d", error);
        }
    },
    // on_receive
    [](TCB* tcb, const uint8_t* data, size_t len) {
        LOG_INFO(APP, "Received %zu bytes", len);
    },
    // on_close
    [](TCB* tcb) {
        LOG_INFO(APP, "Connection closed");
    }
);
```

## 7. 完整协议栈架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         应用层                                   │
│   ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐ │
│   │ Echo Server │  │ HTTP Server │  │   Custom Application    │ │
│   └──────┬──────┘  └──────┬──────┘  └───────────┬─────────────┘ │
│          │                │                     │               │
├──────────┼────────────────┼─────────────────────┼───────────────┤
│          │      传输层     │                     │               │
│   ┌──────▼──────┐  ┌──────▼──────┐   ┌──────────▼──────────┐    │
│   │  UDPLayer   │  │  TCPLayer   │   │                     │    │
│   │ (无状态)     │  │ (有状态)     │   │   (future: SCTP?)   │    │
│   └──────┬──────┘  └──────┬──────┘   └──────────┬──────────┘    │
│          │                │                     │               │
│          └────────────────┼─────────────────────┘               │
│                           │                                     │
│              IProtocolHandler.handle(pkt)                       │
│                           │                                     │
├───────────────────────────┼─────────────────────────────────────┤
│                     网络层 │                                     │
│                   ┌───────▼───────┐                             │
│                   │   IPv4Layer   │                             │
│                   │ ┌───────────┐ │                             │
│                   │ │ handlers  │ │  ICMP=1, TCP=6, UDP=17      │
│                   │ └───────────┘ │                             │
│                   └───────┬───────┘                             │
│                           │                                     │
├───────────────────────────┼─────────────────────────────────────┤
│                     链路层 │                                     │
│                   ┌───────▼───────┐                             │
│                   │   NetDevice   │                             │
│                   │  (TUN/TAP)    │                             │
│                   └───────────────┘                             │
└─────────────────────────────────────────────────────────────────┘
```

## 8. 测试

```bash
# 编译
cd build && cmake .. && make -j4

# 运行
sudo ./neustack -v

# 另一个终端
sudo ifconfig utun4 192.168.100.1 192.168.100.2 up

# 测试
ping 192.168.100.2                      # ICMP
echo "hello" | nc -u 192.168.100.2 7    # UDP
nc 192.168.100.2 7                      # TCP
```

## 9. 总结

### 9.1 完成的功能

| 层 | 功能 | 状态 |
|----|------|------|
| HAL | TUN/TAP 设备 | ✅ |
| 网络层 | IPv4 收发、路由 | ✅ |
| 网络层 | ICMP Ping | ✅ |
| 传输层 | UDP | ✅ |
| 传输层 | TCP 连接管理 | ✅ |
| 传输层 | TCP 可靠传输 | ✅ (基础) |
| 传输层 | TCP 拥塞控制 | ✅ (接口) |
| 传输层 | TCPLayer 封装 | ✅ |

### 9.2 可扩展方向

1. **ARP 支持**：目前只在点对点 TUN 上工作
2. **IP 分片**：支持大于 MTU 的包
3. **更多拥塞控制算法**：CUBIC、BBR
4. **TCP 选项**：MSS、Window Scaling、Timestamps、SACK
5. **性能优化**：零拷贝、批量处理

### 9.3 学习路径回顾

```
01_hal.md          → 设备抽象
02_ipv4.md         → 网络层基础
03_icmp_arp.md     → ICMP 实现
04_udp.md          → 传输层入门
05_tcp_basics.md   → TCP 数据结构
06_tcp_connection.md → TCP 状态机
07_tcp_reliability.md → 可靠传输
08_tcp_congestion_flow.md → 拥塞/流量控制
09_tcp_layer.md    → 整合封装 (本章)
```

恭喜！你已经实现了一个功能完整的用户态 TCP/IP 协议栈！
