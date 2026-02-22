#ifndef NEUSTACK_TRANSPORT_TCP_LAYER
#define NEUSTACK_TRANSPORT_TCP_LAYER

#include "neustack/net/ipv4.hpp"
#include "neustack/net/protocol_handler.hpp"
#include "neustack/transport/stream.hpp"
#include "neustack/transport/tcp_connection.hpp"
#include "neustack/transport/tcp_segment.hpp"
#include "neustack/common/ring_buffer.hpp"
#include "neustack/common/spsc_queue.hpp"

#ifdef NEUSTACK_AI_ENABLED
#include "neustack/transport/tcp_orca.hpp"
#include "neustack/transport/tcp_cubic.hpp"
#include "neustack/ai/intelligence_plane.hpp"
#include "neustack/ai/ai_agent.hpp"
#include "neustack/common/log.hpp"
#endif

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

#ifdef NEUSTACK_AI_ENABLED
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

    /**
     * @brief 获取 AI Agent（用于 API 查询）
     */
    const NetworkAgent& agent() const { return _agent; }
#else
    bool ai_enabled() const { return false; }
#endif

    /**
     * @brief 获取 TCP 指标缓冲区（用于数据采集导出）
     */
    MetricsBuffer<TCPSample, 1024>& metrics_buffer() { return _metrics_buf; }

    /**
     * @brief 获取连接管理器引用 (Telemetry API 用)
     */
    TCPConnectionManager& connection_manager() { return _tcp_mgr; }

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
    MetricsBuffer<TCPSample, 1024> _metrics_buf; // → 智能面 (also used for data collection)

#ifdef NEUSTACK_AI_ENABLED
    SPSCQueue<AIAction, 16> _action_queue;       // ← 智能面

    // ─── 智能面线程 (用 unique_ptr 延迟初始化) ───
    std::unique_ptr<IntelligencePlane> _ai;

    // ─── AI Agent 处理 ───
    NetworkAgent _agent;
    AgentState _last_agent_state = AgentState::NORMAL;  // 追踪状态变化

    // 在事件循环中检查 AI 决策
    void process_ai_actions() {
        AIAction action;
        while (_action_queue.try_pop(action)) {
            switch (action.type) {
                case AIAction::Type::CWND_ADJUST:
                    _agent.on_cwnd_adjust(action.cwnd.alpha);
                    apply_cwnd_adjust();
                    break;
                case AIAction::Type::ANOMALY_ALERT:
                    _agent.on_anomaly(action.anomaly.score);
                    apply_state_change();
                    break;
                case AIAction::Type::BW_PREDICTION:
                    _agent.on_bw_prediction(action.bandwidth.predicted_bw);
                    apply_state_change();
                    break;
                default:
                    break;
            }
        }
    }

    // 将 agent 的 effective_alpha 应用到所有 Orca 连接
    void apply_cwnd_adjust() {
        float alpha = _agent.effective_alpha();
        for (auto& [tuple, tcb_ptr] : _tcp_mgr._connections) {
            auto* orca = dynamic_cast<TCPOrca*>(tcb_ptr->congestion_control.get());
            if (orca) {
                orca->set_alpha(alpha);
            }
        }
    }

    // Agent 状态变化时切换 CC 算法
    void apply_state_change() {
        AgentState current = _agent.state();
        if (current == _last_agent_state) return;

        AgentState prev = _last_agent_state;
        _last_agent_state = current;

        if (current == AgentState::UNDER_ATTACK) {
            // 切回 Cubic：安全、不依赖 AI
            switch_all_cc_to_cubic();
            LOG_WARN(AI, "Agent: UNDER_ATTACK - switched all connections to CUBIC");
        } else if (prev == AgentState::UNDER_ATTACK) {
            // 从攻击恢复，切回 Orca
            switch_all_cc_to_orca();
            LOG_INFO(AI, "Agent: %s - switched all connections to Orca",
                     agent_state_name(current));
        }
    }

    // 切换所有连接到 Cubic
    void switch_all_cc_to_cubic() {
        for (auto& [tuple, tcb_ptr] : _tcp_mgr._connections) {
            if (dynamic_cast<TCPOrca*>(tcb_ptr->congestion_control.get())) {
                tcb_ptr->congestion_control = std::make_unique<TCPCubic>(tcb_ptr->options.mss);
            }
        }
    }

    // 切换所有连接到 Orca
    void switch_all_cc_to_orca() {
        for (auto& [tuple, tcb_ptr] : _tcp_mgr._connections) {
            if (dynamic_cast<TCPCubic*>(tcb_ptr->congestion_control.get())) {
                tcb_ptr->congestion_control = std::make_unique<TCPOrca>(tcb_ptr->options.mss);
            }
        }
    }
#else
    void process_ai_actions() {} // no-op when AI disabled
#endif
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_LAYER