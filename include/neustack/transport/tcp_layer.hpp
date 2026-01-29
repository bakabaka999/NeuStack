#ifndef NEUSTACK_TRANSPORT_TCP_LAYER
#define NEUSTACK_TRANSPORT_TCP_LAYER

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
// 参数：tcb - 新连接
// 返回：该连接的回调函数
using TCPAcceptCallback = std::function<TCPCallbacks(TCB *tcb)>;

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
    TCPLayer(IPv4Layer &ip_layer, uint32_t local_ip);

    // ─── IProtocolHandler 接口 ───

    /**
     * @brief 处理收到的 IPv4 包
     */
    void handle(const IPv4Packet &pkt) override;

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
    ssize_t send(TCB *tcb, const uint8_t *data, size_t len);

    /**
     * @brief 关闭连接
     * @param tcb 连接
     * @return 0 成功，-1 失败
     */
    int close(TCB *tcb);

    // ─── 连接配置 API ───

    /**
     * @brief 设置默认连接选项（新连接会继承）
     * @param opts 选项
     */
    void set_default_options(const TCPOptions &opts) {
        _default_options = opts;
        _tcp_mgr.set_default_options(opts);
    }

    /**
     * @brief 获取默认连接选项
     */
    const TCPOptions &default_options() const { return _default_options; }

    /**
     * @brief 设置单个连接的选项
     * @param tcb 连接
     * @param opts 选项
     */
    static void set_options(TCB *tcb, const TCPOptions &opts) { tcb->apply_options(opts); }

private:
    IPv4Layer &_ip_layer;
    TCPConnectionManager _tcp_mgr;
    TCPOptions _default_options;

    // 监听回调表
    std::unordered_map<uint16_t, TCPAcceptCallback> _accept_callbacks;

    // 临时端口分配
    uint16_t _next_ephemeral_port = 49152;
    uint16_t allocate_ephemeral_port();
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_LAYER