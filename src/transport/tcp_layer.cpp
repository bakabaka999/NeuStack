#include "neustack/transport/tcp_layer.hpp"
#include "neustack/common/log.hpp"
#include "neustack/common/ip_addr.hpp"

namespace neustack {

// ============================================================================
// TCPStreamConnection 实现
// ============================================================================

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

// ============================================================================
// TCPLayer 实现
// ============================================================================

TCPLayer::TCPLayer(IPv4Layer &ip_layer, uint32_t local_ip)
    : _ip_layer(ip_layer), _tcp_mgr(local_ip) {
    // 设置发送回调：TCP -> IP 层
    _tcp_mgr.set_send_callback([this](uint32_t dst_ip, const uint8_t *data, size_t len) {
        _ip_layer.send(dst_ip, static_cast<uint8_t>(IPProtocol::TCP), data, len);
    });

    // 连接 AI 指标采集缓冲区
    _tcp_mgr.set_metrics_buffer(&_metrics_buf);
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

    // 处理 AI 决策
    process_ai_actions();
}

void TCPLayer::handle_icmp_error(const ICMPErrorInfo &error) {
    _tcp_mgr.on_icmp_error(error);
}

#ifdef NEUSTACK_AI_ENABLED
void TCPLayer::enable_ai(const IntelligencePlaneConfig& config) {
    if (_ai) {
        LOG_WARN(TCP, "AI already enabled, stopping first");
        disable_ai();
    }

    _ai = std::make_unique<IntelligencePlane>(_metrics_buf, _action_queue, config);
    _ai->start();
    LOG_INFO(TCP, "AI intelligence plane enabled");
}

void TCPLayer::disable_ai() {
    if (_ai) {
        _ai->stop();
        _ai.reset();
        LOG_INFO(TCP, "AI intelligence plane disabled");
    }
}
#endif

int TCPLayer::listen(uint16_t port, StreamAcceptCallback on_accept) {
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

        // 创建 StreamConnection 包装
        auto conn = get_or_create_connection(tcb);

        // 调用应用层的 accept 回调，获取该连接的回调
        StreamCallbacks callbacks = it->second(conn);

        // 保存回调
        _callbacks[tcb] = std::move(callbacks);

        // 设置底层 TCB 的回调（适配到 Stream 回调）
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
            // 清理
            _callbacks.erase(tcb);
            remove_connection(tcb);
        };

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
                      IStreamClient::ConnectCallback on_connect,
                      std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                      std::function<void(IStreamConnection *)> on_close) {
    // 自动分配本地端口
    if (local_port == 0) {
        local_port = allocate_ephemeral_port();
    }

    // 包装 on_connect
    auto wrapped_connect = [this, on_connect, on_receive, on_close](TCB *tcb, int error) {
        TCPStreamConnection *conn = nullptr;

        if (error == 0) {
            // 连接成功，创建包装
            conn = get_or_create_connection(tcb);

            // 保存回调
            _callbacks[tcb] = StreamCallbacks{on_receive, on_close};

            // 设置底层 TCB 回调
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
        }

        // 通知应用层
        if (on_connect) {
            on_connect(conn, error);
        }
    };

    return _tcp_mgr.connect(remote_ip, remote_port, local_port, wrapped_connect);
}

TCPStreamConnection *TCPLayer:: get_or_create_connection(TCB *tcb) {
    auto it = _connections.find(tcb);
    if (it != _connections.end()) {
        return it->second.get();
    }

    // 创建新的包装
    auto conn = std::make_unique<TCPStreamConnection>(*this, tcb);
    auto *ptr = conn.get();
    _connections[tcb] = std::move(conn);
    return ptr;
}

void TCPLayer::remove_connection(TCB *tcb) {
    _connections.erase(tcb);
}

uint16_t TCPLayer::allocate_ephemeral_port() {
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

    LOG_ERROR(TCP, "No ephemeral ports available");
    return 0;
}

} // namespace neustack
