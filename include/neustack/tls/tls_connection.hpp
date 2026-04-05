#ifndef NEUSTACK_TLS_CONNECTION_HPP
#define NEUSTACK_TLS_CONNECTION_HPP

#ifdef NEUSTACK_TLS_ENABLED

#include "neustack/transport/stream.hpp"
#include "neustack/tls/tls_context.hpp"

#include <mbedtls/ssl.h>

#include <vector>
#include <memory>

namespace neustack {

/**
 * TLS 握手状态
 */
enum class TLSHandshakeState {
    Pending,     // 握手未开始
    InProgress,  // 握手进行中
    Complete,    // 握手完成
    Failed,      // 握手失败
};

/**
 * TLSStreamConnection — 装饰 IStreamConnection，透明加解密
 *
 * BIO 回调对接底层 TCP send/recv，非阻塞握手状态机。
 * 握手完成后，send()/recv() 自动加解密。
 */
class TLSStreamConnection : public IStreamConnection {
public:
    /**
     * @brief 构造 TLS 连接
     * @param inner 底层 TCP 连接（所有权不转移）
     * @param ctx TLS 上下文（共享，不转移所有权）
     */
    TLSStreamConnection(IStreamConnection *inner, TLSContext &ctx);
    ~TLSStreamConnection();

    TLSStreamConnection(const TLSStreamConnection &) = delete;
    TLSStreamConnection &operator=(const TLSStreamConnection &) = delete;

    // ─── IStreamConnection 接口 ───
    ssize_t send(const uint8_t *data, size_t len) override;
    void close() override;
    uint32_t remote_ip() const override;
    uint16_t remote_port() const override;
    StreamError last_error() const override;
    uint8_t last_error_detail() const override;

    // ─── TLS 特有 API ───

    /**
     * @brief 喂入从 TCP 收到的加密数据
     *
     * TLSLayer 收到 on_receive 后调用此方法，数据追加到内部缓冲区，
     * 然后驱动握手或解密。
     *
     * @param data 加密数据
     * @param len 长度
     */
    void on_tcp_data(const uint8_t *data, size_t len);

    /**
     * @brief 驱动 TLS 握手
     * @return 当前握手状态
     */
    TLSHandshakeState drive_handshake();

    /**
     * @brief 获取当前握手状态
     */
    TLSHandshakeState handshake_state() const { return _hs_state; }

    /**
     * @brief 取出已解密的明文数据
     * @param buf 输出缓冲区
     * @param max_len 最大长度
     * @return 读取的字节数，0 = 无数据，<0 = 错误
     */
    int read_decrypted(uint8_t *buf, size_t max_len);

    /**
     * @brief 获取底层连接
     */
    IStreamConnection *inner() const { return _inner; }

    /**
     * @brief 设置 SNI hostname（客户端连接时必须在握手前调用）
     * @param hostname 服务器域名（如 "x.com"）
     */
    void set_hostname(const std::string &hostname);

private:
    // BIO 回调（mbedTLS 用）
    static int bio_send(void *ctx, const unsigned char *buf, size_t len);
    static int bio_recv(void *ctx, unsigned char *buf, size_t len);

    IStreamConnection *_inner;   // 底层 TCP 连接
    TLSContext &_ctx;

    mbedtls_ssl_context _ssl;
    TLSHandshakeState _hs_state = TLSHandshakeState::Pending;
    StreamError _last_error = StreamError::None;

    // 接收缓冲区：TCP 加密数据 → mbedTLS bio_recv 读取
    std::vector<uint8_t> _recv_buf;
    size_t _recv_offset = 0;
};

} // namespace neustack

#endif // NEUSTACK_TLS_ENABLED
#endif // NEUSTACK_TLS_CONNECTION_HPP
