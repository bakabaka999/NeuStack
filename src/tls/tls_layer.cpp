#ifdef NEUSTACK_TLS_ENABLED

#include "neustack/tls/tls_layer.hpp"
#include "neustack/common/log.hpp"

namespace neustack {

// ─── 构造 / 析构 ───

TLSLayer::TLSLayer(IStreamServer &tcp_server,
                    IStreamClient &tcp_client,
                    std::unique_ptr<TLSContext> server_ctx,
                    std::unique_ptr<TLSContext> client_ctx)
    : _tcp_server(tcp_server)
    , _tcp_client(tcp_client)
    , _server_ctx(std::move(server_ctx))
    , _client_ctx(std::move(client_ctx)) {
}

TLSLayer::~TLSLayer() = default;

// ─── IStreamServer: listen ───

int TLSLayer::listen(uint16_t port, StreamAcceptCallback on_accept) {
    _accept_callbacks[port] = std::move(on_accept);

    // 监听底层 TCP，拦截 accept
    return _tcp_server.listen(port, [this, port](IStreamConnection *tcp_conn) -> StreamCallbacks {
        auto it = _accept_callbacks.find(port);
        if (it == _accept_callbacks.end()) {
            LOG_WARN(TLS, "No accept callback for TLS port %u", port);
            return {};
        }

        // 创建 TLS 连接，开始握手
        auto tls_conn = std::make_unique<TLSStreamConnection>(tcp_conn, *_server_ctx);

        auto *tls_raw = tls_conn.get();
        PendingAccept pending;
        pending.tls_conn = std::move(tls_conn);
        pending.app_accept = &it->second;
        _pending_accepts[tcp_conn] = std::move(pending);

        // 立即尝试握手（通常需要等客户端 ClientHello）
        tls_raw->drive_handshake();

        // 返回 TCP 层的回调：将加密数据路由到 TLS 连接
        return StreamCallbacks{
            .on_receive = [this](IStreamConnection *tcp_conn, const uint8_t *data, size_t len) {
                auto pa_it = _pending_accepts.find(tcp_conn);
                if (pa_it != _pending_accepts.end()) {
                    // 握手阶段
                    auto &pa = pa_it->second;
                    pa.tls_conn->on_tcp_data(data, len);
                    auto state = pa.tls_conn->drive_handshake();

                    if (state == TLSHandshakeState::Complete) {
                        // 握手完成！通知应用层
                        auto *tls_raw = pa.tls_conn.get();
                        StreamCallbacks app_cbs = (*pa.app_accept)(tls_raw);

                        // 迁移到 established 表
                        _established[tcp_conn] = std::move(pa.tls_conn);
                        _established_callbacks[tcp_conn] = std::move(app_cbs);
                        _pending_accepts.erase(pa_it);

                        // 握手期间可能已经收到了应用数据，尝试解密
                        uint8_t plaintext[4096];
                        int n = tls_raw->read_decrypted(plaintext, sizeof(plaintext));
                        if (n > 0) {
                            auto &cbs = _established_callbacks[tcp_conn];
                            if (cbs.on_receive) {
                                cbs.on_receive(tls_raw, plaintext, n);
                            }
                        }
                    } else if (state == TLSHandshakeState::Failed) {
                        LOG_WARN(TLS, "TLS handshake failed, closing connection");
                        tcp_conn->close();
                        _pending_accepts.erase(pa_it);
                    }
                    return;
                }

                // 已建立连接：解密数据并转发给应用层
                auto est_it = _established.find(tcp_conn);
                if (est_it != _established.end()) {
                    auto *tls_conn = est_it->second.get();
                    tls_conn->on_tcp_data(data, len);

                    uint8_t plaintext[4096];
                    int n;
                    while ((n = tls_conn->read_decrypted(plaintext, sizeof(plaintext))) > 0) {
                        auto &cbs = _established_callbacks[tcp_conn];
                        if (cbs.on_receive) {
                            cbs.on_receive(tls_conn, plaintext, n);
                        }
                    }
                }
            },
            .on_close = [this](IStreamConnection *tcp_conn) {
                // 清理 pending
                auto pa_it = _pending_accepts.find(tcp_conn);
                if (pa_it != _pending_accepts.end()) {
                    _pending_accepts.erase(pa_it);
                    return;
                }

                // 清理 established
                auto est_it = _established.find(tcp_conn);
                if (est_it != _established.end()) {
                    auto *tls_conn = est_it->second.get();
                    auto &cbs = _established_callbacks[tcp_conn];
                    if (cbs.on_close) {
                        cbs.on_close(tls_conn);
                    }
                    _established_callbacks.erase(tcp_conn);
                    _established.erase(est_it);
                }
            }
        };
    });
}

void TLSLayer::unlisten(uint16_t port) {
    _accept_callbacks.erase(port);
    _tcp_server.unlisten(port);
}

// ─── IStreamClient: connect ───

int TLSLayer::connect(uint32_t remote_ip, uint16_t remote_port,
                       ConnectCallback on_connect,
                       std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                       std::function<void(IStreamConnection *)> on_close) {

    if (!_client_ctx) {
        LOG_ERROR(TLS, "No client TLS context configured");
        return -1;
    }

    // 包装回调：TCP 连接成功后开始 TLS 握手
    auto tls_on_connect = [this, on_connect, on_receive, on_close](
                              IStreamConnection *tcp_conn, int error) {
        if (error != 0) {
            on_connect(nullptr, error);
            return;
        }

        auto tls_conn = std::make_unique<TLSStreamConnection>(tcp_conn, *_client_ctx);

        PendingConnect pending;
        pending.tls_conn = std::move(tls_conn);
        pending.app_on_connect = on_connect;
        pending.app_on_receive = on_receive;
        pending.app_on_close = on_close;
        _pending_connects[tcp_conn] = std::move(pending);

        // 开始 TLS 握手（发 ClientHello）
        _pending_connects[tcp_conn].tls_conn->drive_handshake();
    };

    auto tls_on_receive = [this](IStreamConnection *tcp_conn, const uint8_t *data, size_t len) {
        auto pc_it = _pending_connects.find(tcp_conn);
        if (pc_it != _pending_connects.end()) {
            // 握手阶段
            auto &pc = pc_it->second;
            pc.tls_conn->on_tcp_data(data, len);
            auto state = pc.tls_conn->drive_handshake();

            if (state == TLSHandshakeState::Complete) {
                auto *tls_raw = pc.tls_conn.get();
                auto app_connect = std::move(pc.app_on_connect);
                auto app_receive = std::move(pc.app_on_receive);
                auto app_close = std::move(pc.app_on_close);

                _established[tcp_conn] = std::move(pc.tls_conn);
                _established_callbacks[tcp_conn] = StreamCallbacks{app_receive, app_close};
                _pending_connects.erase(pc_it);

                // 通知应用层连接就绪
                app_connect(tls_raw, 0);

                // 检查是否有残余应用数据
                uint8_t plaintext[4096];
                int n = tls_raw->read_decrypted(plaintext, sizeof(plaintext));
                if (n > 0 && app_receive) {
                    app_receive(tls_raw, plaintext, n);
                }
            } else if (state == TLSHandshakeState::Failed) {
                auto app_connect = std::move(pc.app_on_connect);
                _pending_connects.erase(pc_it);
                app_connect(nullptr, -1);
            }
            return;
        }

        // 已建立连接
        auto est_it = _established.find(tcp_conn);
        if (est_it != _established.end()) {
            auto *tls_conn = est_it->second.get();
            tls_conn->on_tcp_data(data, len);

            uint8_t plaintext[4096];
            int n;
            while ((n = tls_conn->read_decrypted(plaintext, sizeof(plaintext))) > 0) {
                auto &cbs = _established_callbacks[tcp_conn];
                if (cbs.on_receive) {
                    cbs.on_receive(tls_conn, plaintext, n);
                }
            }
        }
    };

    auto tls_on_close = [this](IStreamConnection *tcp_conn) {
        auto pc_it = _pending_connects.find(tcp_conn);
        if (pc_it != _pending_connects.end()) {
            auto app_connect = std::move(pc_it->second.app_on_connect);
            _pending_connects.erase(pc_it);
            app_connect(nullptr, -1);
            return;
        }

        auto est_it = _established.find(tcp_conn);
        if (est_it != _established.end()) {
            auto *tls_conn = est_it->second.get();
            auto &cbs = _established_callbacks[tcp_conn];
            if (cbs.on_close) {
                cbs.on_close(tls_conn);
            }
            _established_callbacks.erase(tcp_conn);
            _established.erase(est_it);
        }
    };

    return _tcp_client.connect(remote_ip, remote_port,
                                tls_on_connect, tls_on_receive, tls_on_close);
}

} // namespace neustack

#endif // NEUSTACK_TLS_ENABLED
