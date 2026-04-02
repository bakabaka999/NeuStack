#ifndef NEUSTACK_TLS_LAYER_HPP
#define NEUSTACK_TLS_LAYER_HPP

#ifdef NEUSTACK_TLS_ENABLED

#include "neustack/transport/stream.hpp"
#include "neustack/tls/tls_context.hpp"
#include "neustack/tls/tls_connection.hpp"

#include <memory>
#include <unordered_map>
#include <functional>

namespace neustack {

/**
 * TLSLayer — 在 TCPLayer 和 HttpServer/HttpClient 之间的 TLS 中间层
 *
 * 实现 IStreamServer + IStreamClient，编排 TLS 握手流程。
 * 握手完成后才通知上层（对应用层完全透明）。
 *
 * 架构：
 *   HttpServer / HttpClient
 *          ↓ IStreamServer / IStreamClient
 *      TLSLayer (本类)
 *          ↓ IStreamServer / IStreamClient
 *      TCPLayer
 */
class TLSLayer : public IStreamServer, public IStreamClient {
public:
    /**
     * @brief 构造 TLS 层
     * @param tcp_server 底层 TCP 服务端接口
     * @param tcp_client 底层 TCP 客户端接口
     * @param server_ctx Server TLS 上下文（listen 时使用）
     * @param client_ctx Client TLS 上下文（connect 时使用，可选）
     */
    TLSLayer(IStreamServer &tcp_server,
             IStreamClient &tcp_client,
             std::unique_ptr<TLSContext> server_ctx,
             std::unique_ptr<TLSContext> client_ctx = nullptr);

    ~TLSLayer();

    // ─── IStreamServer 接口 ───
    int listen(uint16_t port, StreamAcceptCallback on_accept) override;
    void unlisten(uint16_t port) override;

    // ─── IStreamClient 接口 ───
    int connect(uint32_t remote_ip, uint16_t remote_port,
                ConnectCallback on_connect,
                std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                std::function<void(IStreamConnection *)> on_close) override;

    /**
     * @brief 获取 Server TLS 上下文
     */
    TLSContext *server_context() const { return _server_ctx.get(); }

    /**
     * @brief 获取 Client TLS 上下文
     */
    TLSContext *client_context() const { return _client_ctx.get(); }

    /**
     * @brief 设置下一次 connect() 使用的 SNI hostname
     *
     * 必须在 connect() 之前调用。连接发起后自动清除。
     * 公网 HTTPS 服务器（如 Cloudflare）要求 SNI 才能正确响应。
     */
    void set_next_hostname(const std::string &hostname) { _next_hostname = hostname; }

private:
    IStreamServer &_tcp_server;
    IStreamClient &_tcp_client;
    std::unique_ptr<TLSContext> _server_ctx;
    std::unique_ptr<TLSContext> _client_ctx;
    std::string _next_hostname;  // SNI for next connect()

    // 服务端: port → 应用层 accept 回调
    std::unordered_map<uint16_t, StreamAcceptCallback> _accept_callbacks;

    // 活跃 TLS 连接（inner TCP conn → TLS conn）
    // 服务端握手状态追踪
    struct PendingAccept {
        std::unique_ptr<TLSStreamConnection> tls_conn;
        StreamAcceptCallback *app_accept;       // 指向 _accept_callbacks 中的回调
        StreamCallbacks app_callbacks;           // 握手完成后存放应用层回调
        bool handshake_complete = false;
    };
    std::unordered_map<IStreamConnection *, PendingAccept> _pending_accepts;

    // 已完成握手的连接
    std::unordered_map<IStreamConnection *, std::unique_ptr<TLSStreamConnection>> _established;
    std::unordered_map<IStreamConnection *, StreamCallbacks> _established_callbacks;

    // 客户端握手状态追踪
    struct PendingConnect {
        std::unique_ptr<TLSStreamConnection> tls_conn;
        ConnectCallback app_on_connect;
        std::function<void(IStreamConnection *, const uint8_t *, size_t)> app_on_receive;
        std::function<void(IStreamConnection *)> app_on_close;
    };
    std::unordered_map<IStreamConnection *, PendingConnect> _pending_connects;
};

} // namespace neustack

#endif // NEUSTACK_TLS_ENABLED
#endif // NEUSTACK_TLS_LAYER_HPP
