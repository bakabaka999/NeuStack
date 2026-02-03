#ifndef NEUSTACK_TRANSPORT_TCP_LAYER
#define NEUSTACK_TRANSPORT_TCP_LAYER

#include "neustack/net/ipv4.hpp"
#include "neustack/net/protocol_handler.hpp"
#include "neustack/transport/stream.hpp"
#include "neustack/transport/tcp_connection.hpp"
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/common/ring_buffer.hpp"
#include "neustack/common/spsc_queue.hpp"
#include "neustack/ai/intelligence_plane.hpp"

#include <memory>

namespace neustack {

// ============================================================================
// TCPStreamConnection - TCP 连接的 IStreamConnection 适配器
// ============================================================================

class TCPLayer;  // 前向声明

class TCPStreamConnection : public IStreamConnection {
public:
    TCPStreamConnection(TCPLayer &layer, TCB *tcb) : _layer(layer), _tcb(tcb) {}

    ssize_t send(const uint8_t *data, size_t len) override;
    void close() override;

    uint32_t remote_ip() const override;
    uint16_t remote_port() const override;

    // 获取底层 TCB（内部使用）
    TCB *tcb() const { return _tcb; }

private:
    TCPLayer &_layer;
    TCB *_tcb;
};

// ============================================================================
// TCPLayer - TCP 传输层
// ============================================================================

class TCPLayer : public IProtocolHandler, public IStreamServer, public IStreamClient {
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

    // ─── IStreamServer 接口 ───

    /**
     * @brief 监听端口
     * @param port 本地端口
     * @param on_accept 新连接回调
     * @return 0 成功，-1 失败
     */
    int listen(uint16_t port, StreamAcceptCallback on_accept) override;

    /**
     * @brief 停止监听
     * @param port 端口号
     */
    void unlisten(uint16_t port) override;

    // ─── 客户端 API ───

    /**
     * @brief 连接远程主机 (IStreamClient 接口)
     */
    int connect(uint32_t remote_ip, uint16_t remote_port,
                IStreamClient::ConnectCallback on_connect,
                std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                std::function<void(IStreamConnection *)> on_close) override {
        return connect(remote_ip, remote_port, 0, on_connect, on_receive, on_close);
    }

    /**
     * @brief 连接远程主机（指定本地端口）
     * @param remote_ip 远程 IP
     * @param remote_port 远程端口
     * @param local_port 本地端口（0 = 自动分配）
     * @param on_connect 连接完成回调
     * @param on_receive 数据接收回调
     * @param on_close 连接关闭回调
     * @return 0 成功（异步），-1 失败
     */
    int connect(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port,
                IStreamClient::ConnectCallback on_connect,
                std::function<void(IStreamConnection *, const uint8_t *, size_t)> on_receive,
                std::function<void(IStreamConnection *)> on_close);

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

    // ─── AI 控制 API ───

    /**
     * @brief 启用 AI 智能面
     * @param config AI 配置（模型路径等）
     */
    void enable_ai(const IntelligencePlaneConfig& config);

    /**
     * @brief 禁用 AI 智能面
     */
    void disable_ai();

    /**
     * @brief AI 是否已启用
     */
    bool ai_enabled() const { return _ai != nullptr && _ai->is_running(); }

private:
    friend class TCPStreamConnection;  // 允许访问 _tcp_mgr

    IPv4Layer &_ip_layer;
    TCPConnectionManager _tcp_mgr;
    TCPOptions _default_options;

    // Stream 接口的回调和连接管理
    std::unordered_map<uint16_t, StreamAcceptCallback> _accept_callbacks;
    std::unordered_map<TCB *, std::unique_ptr<TCPStreamConnection>> _connections;
    std::unordered_map<TCB *, StreamCallbacks> _callbacks;

    // 内部：通过 TCB 查找或创建 TCPStreamConnection
    TCPStreamConnection *get_or_create_connection(TCB *tcb);
    void remove_connection(TCB *tcb);

    // 临时端口分配
    uint16_t _next_ephemeral_port = 49152;
    uint16_t allocate_ephemeral_port();

    // AI 相关的字段与方法
    // ─── AI 通信通道 ───
    MetricsBuffer<TCPSample, 1024> _metrics_buf; // → 智能面
    SPSCQueue<AIAction, 16> _action_queue;       // ← 智能面

    // ─── 智能面线程 (用 unique_ptr 延迟初始化) ───
    std::unique_ptr<IntelligencePlane> _ai;

    // 在事件循环中检查 AI 决策
    void process_ai_actions() {
        AIAction action;
        while (_action_queue.try_pop(action)) {
            switch (action.type) {
                case AIAction::Type::CWND_ADJUST:
                    apply_cwnd_action(action);
                    break;
                case AIAction::Type::ANOMALY_ALERT:
                    handle_anomaly(action);
                    break;
                default:
                    break;
            }
        }
    }

    // AI 动作处理 (待实现)
    void apply_cwnd_action(const AIAction& action) {
        // TODO: 根据 action.conn_id 找到对应 TCB，调整 cwnd
        (void)action;
    }

    void handle_anomaly(const AIAction& action) {
        // TODO: 记录日志，触发告警
        (void)action;
    }
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_LAYER