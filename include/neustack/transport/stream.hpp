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
