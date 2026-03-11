#ifndef NEUSTACK_TELEMETRY_TELEMETRY_API_HPP
#define NEUSTACK_TELEMETRY_TELEMETRY_API_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <memory>

namespace neustack {
    struct GlobalMetrics;
    class SecurityMetrics;
    class TCPConnectionManager;
    class FirewallEngine;
    class NetworkAgent;
}

namespace neustack::telemetry {
    class MetricsRegistry; // Inside telemetry namespace

// ─── 聚合状态结构体 ───

/**
 * 协议栈整体状态快照
 *
 * 一次调用获取所有核心指标，避免多次原子读取的时间差异。
 * CLI 工具和 HTTP API 都用这个。
 *
 * 内存布局: 约 280 字节 (不含 string 堆分配)
 * 含 string 成员 (agent_state, orca_status 等) 约 400 字节峰值
 */
struct StackStatus {
    // 时间信息
    std::chrono::steady_clock::time_point timestamp;
    uint64_t uptime_seconds;

    // ─── 流量统计 ───
    struct Traffic {
        uint64_t packets_rx;       // 累计收包数
        uint64_t packets_tx;       // 累计发包数
        uint64_t bytes_rx;         // 累计收字节
        uint64_t bytes_tx;         // 累计发字节

        // 速率 (需要两次快照计算差值，首次调用为 0)
        double pps_rx;             // packets/s 收
        double pps_tx;             // packets/s 发
        double bps_rx;             // bytes/s 收
        double bps_tx;             // bytes/s 发
    } traffic;

    // ─── TCP 连接 ───
    struct TCP {
        uint32_t active_connections;     // 当前活跃连接数
        uint64_t total_established;      // 累计建立连接数
        uint64_t total_reset;            // 累计 RST 数
        uint64_t total_timeout;          // 累计超时数

        // RTT 统计 (从 Histogram 快照计算)
        struct RTT {
            double min_us;       // 最小桶下界 (近似)
            double avg_us;       // sum / count
            double p50_us;       // 中位数 (线性插值)
            double p90_us;
            double p99_us;
            double max_us;       // 最大桶上界 (近似)
            uint64_t samples;    // 总 RTT 样本数
        } rtt;

        // 拥塞控制聚合
        double avg_cwnd;              // 所有连接平均 cwnd
        uint64_t total_retransmits;   // 累计重传次数
    } tcp;

    // ─── 安全 / 防火墙 ───
    struct Security {
        bool firewall_enabled;      // 防火墙是否启用
        bool shadow_mode;           // 是否为影子模式 (只告警不丢包)
        bool ai_enabled;            // AI 安全模型是否加载

        double pps;                 // 安全面 PPS (滑动窗口)
        double syn_rate;            // SYN 包速率
        double syn_synack_ratio;    // SYN/SYN-ACK 比值 (>3 可能是 SYN flood)
        double rst_ratio;           // RST 占总包比例

        uint64_t packets_dropped;   // 被防火墙丢弃的包数
        uint64_t packets_alerted;   // 被防火墙告警的包数

        // AI 状态
        float anomaly_score;              // 异常检测分数 [0, 1]
        std::string agent_state;          // "NORMAL", "DEGRADED", "ATTACK", "RECOVERING"
        float predicted_bandwidth_bps;    // LSTM 预测带宽
    } security;

    // ─── AI 智能面 ───
    struct AI {
        bool enabled;                   // 是否启用 AI 模块
        std::string orca_status;        // "loaded" / "disabled" / "error"
        std::string anomaly_status;     // "loaded" / "disabled" / "error"
        std::string bandwidth_status;   // "loaded" / "disabled" / "error"
        float current_alpha;            // Orca 当前输出 (拥塞控制系数)
    } ai;
};

/**
 * 单个 TCP 连接详情
 *
 * 从 TCB (Transmission Control Block) 中提取的外部可见信息。
 * 不暴露 TCB 内部实现细节（如重传队列、定时器状态）。
 */
struct ConnectionDetail {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    std::string state;          // "ESTABLISHED", "CLOSE_WAIT", "SYN_SENT", etc.

    uint32_t rtt_us;            // 最近一次 RTT (微秒)
    uint32_t srtt_us;           // 平滑 RTT (微秒)
    uint32_t cwnd;              // 拥塞窗口 (段数)
    uint32_t bytes_in_flight;   // 在途字节数
    uint32_t send_buffer_used;  // 发送缓冲区已用字节
    uint32_t recv_buffer_used;  // 接收缓冲区已用字节

    uint64_t bytes_sent;        // 该连接累计发送字节
    uint64_t bytes_received;    // 该连接累计接收字节

    std::chrono::steady_clock::time_point established_at;  // 连接建立时间
};

// ─── 查询 API ───

class TelemetryAPI {
public:
    /**
     * 构造时传入所需引用
     * 由 NeuStack::create() 内部构造，不对外暴露构造函数参数
     */
    TelemetryAPI(neustack::GlobalMetrics& gm,
                 const neustack::SecurityMetrics* sm,
                 neustack::TCPConnectionManager& tcp_mgr,
                 neustack::FirewallEngine* fw,
                 const neustack::NetworkAgent* agent,
                 MetricsRegistry& registry,
                 std::chrono::steady_clock::time_point start_time);

    ~TelemetryAPI();

    // 禁止拷贝/移动 (持有引用)
    TelemetryAPI(const TelemetryAPI&) = delete;
    TelemetryAPI& operator=(const TelemetryAPI&) = delete;

    // ─── 聚合查询 ───

    /** 获取完整状态快照 (约 1-5μs，取决于连接数) */
    StackStatus status();

    /** 只获取流量统计 (最快，约 0.2μs，只读 atomic) */
    StackStatus::Traffic traffic();

    /** 只获取 TCP 统计 (约 0.5-2μs，需要遍历连接算平均 cwnd) */
    StackStatus::TCP tcp_stats();

    /** 只获取安全统计 (约 0.3μs，SecurityMetrics::snapshot 内部有 spinlock) */
    StackStatus::Security security_stats();

    // ─── 连接列表 ───

    /** 获取所有活跃 TCP 连接 (持锁遍历，O(N)) */
    std::vector<ConnectionDetail> connections();

    /** 获取指定远端 IP 的连接 */
    std::vector<ConnectionDetail> connections_by_ip(uint32_t remote_ip);

    /** 获取指定本地端口的连接 */
    std::vector<ConnectionDetail> connections_by_port(uint16_t local_port);

    // ─── 导出 ───

    /** JSON 格式导出完整状态 */
    std::string to_json(bool pretty = false);

    /** Prometheus 格式导出 */
    std::string to_prometheus();

    // ─── 速率计算 ───

    /**
     * 更新速率计算
     *
     * 内部维护上一次快照和时间戳，自动计算 delta/interval。
     * 由 status() / traffic() 自动调用，也可手动调用。
     * 首次调用返回零速率。
     *
     * 推荐调用间隔: 1s。间隔太短 (<100ms) 速率波动大，太长 (>10s) 延迟高。
     */
    void update_rates();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace neustack::telemetry


#endif // NEUSTACK_TELEMETRY_TELEMETRY_API_HPP