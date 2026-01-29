#include "neustack/transport/tcp_layer.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"

namespace neustack {

TCPLayer::TCPLayer(IPv4Layer &ip_layer, uint32_t local_ip)
    : _ip_layer(ip_layer)
    , _tcp_mgr(local_ip)
{
    // 设置发送回调：TCP -> IP 层
    _tcp_mgr.set_send_callback([this](uint32_t dst_ip, const uint8_t* data, size_t len) {
        _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::TCP), data, len);
    });
}

void TCPLayer::handle(const IPv4Packet &pkt) {
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
    int result = _tcp_mgr.listen(port, [this, port](TCB *tcb, int error) {
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
    _tcp_mgr.unlisten(port);
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
    // 从 49152 开始，找一个未被使用的端口
    // 最多尝试 (65535 - 49152 + 1) = 16384 次
    constexpr uint16_t EPHEMERAL_START = 49152;
    constexpr uint16_t EPHEMERAL_END = 65535;
    constexpr int MAX_ATTEMPTS = EPHEMERAL_END - EPHEMERAL_START + 1;

    for (int i = 0; i < MAX_ATTEMPTS; ++i) {
        uint16_t port = _next_ephemeral_port++;
        if (_next_ephemeral_port > EPHEMERAL_END) {
            _next_ephemeral_port = EPHEMERAL_START;
        }

        if (!_tcp_mgr.is_port_in_use(port)) {
            return port;
        }
    }

    // 所有端口都被占用（极端情况）
    LOG_ERROR(TCP, "No ephemeral ports available");
    return 0;
}

} // namespace neustack
