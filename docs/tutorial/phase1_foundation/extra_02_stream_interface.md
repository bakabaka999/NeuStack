# Extra 2: Stream 抽象接口

## 概述

在构建应用层协议（如 HTTP）时，我们遇到一个设计问题：HTTP 代码应该直接依赖 `TCPLayer` 吗？如果将来我们想用 TLS、QUIC 或其他传输方式，是否需要重写 HTTP 模块？

答案是：**不应该**。通过引入抽象接口，我们可以让应用层与传输层解耦，实现真正的协议分层。

## 问题分析

### 直接依赖的问题

```cpp
// 不好的设计：HTTP 直接依赖 TCP
class HttpServer {
    TCPLayer& _tcp;  // 紧耦合
public:
    explicit HttpServer(TCPLayer& tcp) : _tcp(tcp) {}
    // ...
};
```

问题：
1. **无法替换传输层**：想用 TLS？需要修改 HttpServer
2. **测试困难**：必须有完整 TCP 栈才能测试 HTTP
3. **违反依赖倒置原则**：高层模块依赖低层模块的具体实现

### 抽象接口的好处

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Layer                       │
│                   (HTTP, WebSocket, ...)                    │
├─────────────────────────────────────────────────────────────┤
│                    IStreamServer/Client                     │ ← 抽象接口
├─────────────────────────────────────────────────────────────┤
│                     Transport Layer                         │
│            TCPLayer    TLSLayer    QUICLayer               │
└─────────────────────────────────────────────────────────────┘
```

HTTP 只依赖 `IStreamServer` 接口：
- 换 TCP？换 TLS？换 QUIC？HTTP 代码不变
- 测试时可以用 Mock 实现
- 符合 SOLID 原则

## 接口设计

### 核心概念

1. **IStreamConnection**：代表一个字节流连接
2. **StreamCallbacks**：连接的事件回调
3. **IStreamServer**：服务端接口（监听端口，接受连接）
4. **IStreamClient**：客户端接口（主动连接）

### 文件结构

```
include/neustack/transport/
├── stream.hpp           # 抽象接口定义
├── tcp_layer.hpp        # TCP 实现
└── (future) tls_layer.hpp  # TLS 实现
```

## 实现

### stream.hpp - 抽象接口

```cpp
// include/neustack/transport/stream.hpp
#ifndef NEUSTACK_TRANSPORT_STREAM_HPP
#define NEUSTACK_TRANSPORT_STREAM_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <sys/types.h>

namespace neustack {

/**
 * 字节流连接接口
 *
 * 应用层（如 HTTP）只通过这个接口操作连接，
 * 不关心底层是 TCP、TLS 还是 QUIC。
 */
class IStreamConnection {
public:
    virtual ~IStreamConnection() = default;

    /**
     * @brief 发送数据
     * @param data 数据指针
     * @param len 数据长度
     * @return 发送的字节数，-1 表示失败
     */
    virtual ssize_t send(const uint8_t *data, size_t len) = 0;

    /**
     * @brief 关闭连接
     */
    virtual void close() = 0;

    /**
     * @brief 获取远端 IP（可选，用于日志等）
     */
    virtual uint32_t remote_ip() const { return 0; }

    /**
     * @brief 获取远端端口（可选）
     */
    virtual uint16_t remote_port() const { return 0; }
};

/**
 * 连接回调
 */
struct StreamCallbacks {
    std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive;
    std::function<void(IStreamConnection *)> on_close;
};

/**
 * 接受连接回调
 * 参数: 新连接
 * 返回: 该连接的回调
 */
using StreamAcceptCallback = std::function<StreamCallbacks(IStreamConnection *)>;

/**
 * 流式传输服务端接口
 *
 * HTTP 服务器只依赖这个接口，可以跑在：
 * - TCP (HTTP/1.1, HTTP/2)
 * - TLS (HTTPS)
 * - QUIC (HTTP/3)
 */
class IStreamServer {
public:
    virtual ~IStreamServer() = default;

    /**
     * @brief 监听端口
     * @param port 端口号
     * @param on_accept 新连接回调
     * @return 0 成功，-1 失败
     */
    virtual int listen(uint16_t port, StreamAcceptCallback on_accept) = 0;

    /**
     * @brief 停止监听
     * @param port 端口号
     */
    virtual void unlisten(uint16_t port) = 0;
};

/**
 * 流式传输客户端接口（可选，用于 HTTP 客户端）
 */
class IStreamClient {
public:
    virtual ~IStreamClient() = default;

    /**
     * 连接回调
     * @param conn 连接（成功时非空）
     * @param error 0 成功，-1 失败
     */
    using ConnectCallback = std::function<void(IStreamConnection *conn, int error)>;

    /**
     * @brief 连接远程主机
     * @param remote_ip 远程 IP
     * @param remote_port 远程端口
     * @param on_connect 连接完成回调
     * @param on_receive 数据接收回调
     * @param on_close 连接关闭回调
     * @return 0 成功（异步），-1 失败
     */
    virtual int connect(uint32_t remote_ip, uint16_t remote_port,
                        ConnectCallback on_connect,
                        std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                        std::function<void(IStreamConnection *)> on_close) = 0;
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_STREAM_HPP
```

### TCPLayer 实现 IStreamServer

`TCPLayer` 需要实现 `IStreamServer` 接口，同时保持原有的 `IProtocolHandler` 功能：

```cpp
// include/neustack/transport/tcp_layer.hpp
class TCPStreamConnection : public IStreamConnection {
public:
    TCPStreamConnection(TCPLayer &layer, TCB *tcb) : _layer(layer), _tcb(tcb) {}

    ssize_t send(const uint8_t *data, size_t len) override;
    void close() override;
    uint32_t remote_ip() const override;
    uint16_t remote_port() const override;

    TCB *tcb() const { return _tcb; }

private:
    TCPLayer &_layer;
    TCB *_tcb;
};

class TCPLayer : public IProtocolHandler, public IStreamServer {
public:
    TCPLayer(IPv4Layer &ip_layer, uint32_t local_ip);

    // IProtocolHandler 接口
    void handle(const IPv4Packet &pkt) override;

    // IStreamServer 接口
    int listen(uint16_t port, StreamAcceptCallback on_accept) override;
    void unlisten(uint16_t port) override;

    // 定时器
    void on_timer();

    // 客户端 API
    int connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                IStreamClient::ConnectCallback on_connect,
                std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                std::function<void(IStreamConnection *)> on_close);

private:
    friend class TCPStreamConnection;  // 允许访问私有成员

    IPv4Layer &_ip_layer;
    TCPConnectionManager _tcp_mgr;
    TCPOptions _default_options;

    // Stream 接口的回调和连接管理
    std::unordered_map<uint16_t, StreamAcceptCallback> _accept_callbacks;
    std::unordered_map<TCB *, std::unique_ptr<TCPStreamConnection>> _connections;
    std::unordered_map<TCB *, StreamCallbacks> _callbacks;

    TCPStreamConnection *get_or_create_connection(TCB *tcb);
    void remove_connection(TCB *tcb);
};
```

### 适配器模式

`TCPStreamConnection` 是一个适配器，将底层 `TCB` 包装为 `IStreamConnection`：

```cpp
// src/transport/tcp_layer.cpp

ssize_t TCPStreamConnection::send(const uint8_t *data, size_t len) {
    return _layer._tcp_mgr.send(_tcb, data, len);
}

void TCPStreamConnection::close() {
    _layer._tcp_mgr.close(_tcb);
}

uint32_t TCPStreamConnection::remote_ip() const {
    return _tcb->t_tuple.remote_ip;
}

uint16_t TCPStreamConnection::remote_port() const {
    return _tcb->t_tuple.remote_port;
}
```

### listen() 实现

关键是将底层 TCB 回调适配到 Stream 回调：

```cpp
int TCPLayer::listen(uint16_t port, StreamAcceptCallback on_accept) {
    // 保存 accept 回调
    _accept_callbacks[port] = std::move(on_accept);

    // 调用底层 listen，设置 on_connect 回调
    int result = _tcp_mgr.listen(port, [this, port](TCB *tcb, int error) {
        if (error != 0) {
            return;
        }

        // 查找 accept 回调
        auto it = _accept_callbacks.find(port);
        if (it == _accept_callbacks.end()) {
            return;
        }

        // 创建 StreamConnection 包装
        auto conn = get_or_create_connection(tcb);

        // 调用应用层的 accept 回调，获取该连接的回调
        StreamCallbacks callbacks = it->second(conn);

        // 保存回调
        _callbacks[tcb] = std::move(callbacks);

        // 设置底层 TCB 的回调
        tcb->on_receive = [this, tcb](TCB *, const uint8_t *data, size_t len) {
            auto cb_it = _callbacks.find(tcb);
            if (cb_it != _callbacks.end() && cb_it->second.on_receive) {
                auto conn = get_or_create_connection(tcb);
                cb_it->second.on_receive(conn, data, len);
            }
        };

        tcb->on_close = [this, tcb](TCB *) {
            auto cb_it = _callbacks.find(tcb);
            if (cb_it != _callbacks.end() && cb_it->second.on_close) {
                auto conn = get_or_create_connection(tcb);
                cb_it->second.on_close(conn);
            }
            _callbacks.erase(tcb);
            remove_connection(tcb);
        };
    });

    if (result < 0) {
        _accept_callbacks.erase(port);
    }

    return result;
}
```

## 使用示例

### Echo 服务器

使用 Stream 接口的 Echo 服务器，与传输层实现解耦：

```cpp
// 使用 IStreamServer 接口
void start_echo_server(IStreamServer& server, uint16_t port) {
    server.listen(port, [](IStreamConnection* conn) -> StreamCallbacks {
        LOG_INFO(APP, "New connection from %s:%u",
                 ip_to_string(conn->remote_ip()).c_str(),
                 conn->remote_port());

        return StreamCallbacks{
            .on_receive = [](IStreamConnection* conn, const uint8_t* data, size_t len) {
                // Echo: 原样返回数据
                conn->send(data, len);
            },
            .on_close = [](IStreamConnection* conn) {
                LOG_INFO(APP, "Connection closed");
                conn->close();
            }
        };
    });
}

// main.cpp 中使用
int main() {
    // ... 初始化 HAL, IP 层 ...

    TCPLayer tcp_layer(ip_layer, local_ip);
    ip_layer.register_handler(static_cast<uint8_t>(IPProtocol::TCP), &tcp_layer);

    // TCP 层实现了 IStreamServer，直接传入
    start_echo_server(tcp_layer, 7);

    // 主循环 ...
}
```

### HTTP 服务器

HTTP 服务器只依赖 `IStreamServer`，不关心底层是 TCP 还是 TLS：

```cpp
// include/neustack/app/http_server.hpp
class HttpServer {
public:
    // 依赖抽象接口，而非具体实现
    explicit HttpServer(IStreamServer& transport) : _transport(transport) {}

    int listen(uint16_t port);
    // ...

private:
    IStreamServer& _transport;  // 抽象接口
};
```

使用时：

```cpp
// HTTP over TCP
TCPLayer tcp(ip_layer, local_ip);
HttpServer http_tcp(tcp);
http_tcp.listen(80);

// HTTP over TLS (将来)
TLSLayer tls(tcp);
HttpServer http_tls(tls);
http_tls.listen(443);

// HTTP over QUIC (将来)
QUICLayer quic(udp_layer);
HttpServer http_quic(quic);
http_quic.listen(443);
```

## 设计要点

### 1. 回调式 API

为什么使用回调而不是同步 API？

```cpp
// 同步 API（阻塞式）
Connection* conn = server.accept();  // 阻塞等待
ssize_t n = conn->recv(buf, len);    // 阻塞读取

// 回调 API（非阻塞式）
server.listen(port, [](IStreamConnection* conn) {
    return StreamCallbacks{
        .on_receive = [](IStreamConnection* c, const uint8_t* d, size_t l) { ... }
    };
});
```

回调式更适合用户态协议栈：
- 单线程事件循环，无需多线程
- 与底层 TCP 状态机自然契合
- 避免阻塞系统调用

### 2. 连接生命周期

```
StreamAcceptCallback          StreamCallbacks
      │                            │
      ▼                            ▼
[新连接到来] ──────────────► [返回回调]
                                   │
                                   ▼
                           ┌──────────────┐
                           │  on_receive  │ ←── 数据到达
                           │  on_close    │ ←── 连接关闭
                           └──────────────┘
```

- `StreamAcceptCallback`：新连接到来时调用，返回该连接的回调
- `StreamCallbacks.on_receive`：数据到达时调用
- `StreamCallbacks.on_close`：连接关闭时调用

### 3. 错误处理

```cpp
// send() 返回值
ssize_t n = conn->send(data, len);
if (n < 0) {
    // 发送失败
} else if (n < len) {
    // 部分发送（缓冲区满）
}

// listen() 返回值
if (server.listen(port, callback) < 0) {
    // 监听失败（端口被占用等）
}
```

## 测试与 Mock

抽象接口的一个重要好处是便于测试：

```cpp
// 测试用 Mock 实现
class MockStreamConnection : public IStreamConnection {
public:
    std::vector<uint8_t> sent_data;

    ssize_t send(const uint8_t *data, size_t len) override {
        sent_data.insert(sent_data.end(), data, data + len);
        return len;
    }

    void close() override { closed = true; }

    bool closed = false;
};

class MockStreamServer : public IStreamServer {
public:
    int listen(uint16_t port, StreamAcceptCallback on_accept) override {
        _callbacks[port] = std::move(on_accept);
        return 0;
    }

    // 测试辅助：模拟新连接
    void simulate_connection(uint16_t port, MockStreamConnection* conn) {
        auto it = _callbacks.find(port);
        if (it != _callbacks.end()) {
            it->second(conn);
        }
    }

private:
    std::unordered_map<uint16_t, StreamAcceptCallback> _callbacks;
};

// 测试 HTTP 服务器
TEST(HttpServer, BasicRequest) {
    MockStreamServer mock_server;
    HttpServer http(mock_server);
    http.listen(80);

    MockStreamConnection mock_conn;
    mock_server.simulate_connection(80, &mock_conn);

    // 模拟 HTTP 请求
    std::string request = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
    // ... 调用 on_receive ...

    // 验证响应
    EXPECT_TRUE(mock_conn.sent_data.size() > 0);
}
```

## 扩展：TLS 层

将来实现 TLS 时，只需要实现 `IStreamServer` 接口：

```cpp
class TLSLayer : public IStreamServer {
public:
    // 依赖底层 TCP
    TLSLayer(IStreamServer& tcp, const TLSConfig& config);

    // 实现 IStreamServer
    int listen(uint16_t port, StreamAcceptCallback on_accept) override;
    void unlisten(uint16_t port) override;

private:
    IStreamServer& _tcp;
    // TLS 握手状态、证书等
};
```

HTTP 代码无需任何修改即可运行在 TLS 上。

## 小结

Stream 抽象接口的设计遵循了几个重要原则：

1. **依赖倒置**：高层模块（HTTP）依赖抽象（IStreamServer），不依赖具体实现（TCPLayer）
2. **接口隔离**：接口只暴露必要的方法（send, close, listen）
3. **适配器模式**：TCPStreamConnection 适配 TCB 到 IStreamConnection
4. **可测试性**：抽象接口便于 Mock 测试

这种设计让我们的协议栈具备了良好的扩展性，为将来支持 TLS、QUIC 等协议打下基础。
