#include "neustack/telemetry/telemetry_api.hpp"
#include "neustack/telemetry/metrics_registry.hpp"
#include "neustack/telemetry/json_exporter.hpp"
#include "neustack/telemetry/prometheus_exporter.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include "neustack/metrics/security_metrics.hpp"
#include "neustack/transport/tcp_connection.hpp"
#include "neustack/firewall/firewall_engine.hpp"
#include "neustack/ai/ai_agent.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>

using namespace neustack::telemetry;

struct TelemetryAPI::Impl {
    // 引用: 在 NeuStack::Impl 中都已存在，只需传递
    neustack::GlobalMetrics &global_metrics;
    const neustack::SecurityMetrics *security_metrics; // nullable: 防火墙未启用
    neustack::TCPConnectionManager &tcp_mgr;
    neustack::FirewallEngine *firewall; // nullable: 防火墙未启用
    const neustack::NetworkAgent *agent;      // nullable: AI 未启用
    MetricsRegistry &registry;

    // 启动时间 (用于计算 uptime)
    std::chrono::steady_clock::time_point start_time;

    // 速率计算状态
    struct RateState
    {
        uint64_t prev_packets_rx = 0;
        uint64_t prev_packets_tx = 0;
        uint64_t prev_bytes_rx = 0;
        uint64_t prev_bytes_tx = 0;
        std::chrono::steady_clock::time_point prev_time;
        bool initialized = false;

        // 缓存的速率值
        double pps_rx = 0;
        double pps_tx = 0;
        double bps_rx = 0;
        double bps_tx = 0;
    } rate_state;
};

// ─── 构造/析构 ───

TelemetryAPI::TelemetryAPI(neustack::GlobalMetrics& gm,
                           const neustack::SecurityMetrics* sm,
                           neustack::TCPConnectionManager& tcp_mgr,
                           neustack::FirewallEngine* fw,
                           const neustack::NetworkAgent* agent,
                           MetricsRegistry& registry,
                           std::chrono::steady_clock::time_point start_time)
    : _impl(std::make_unique<Impl>(Impl{
          .global_metrics = gm,
          .security_metrics = sm,
          .tcp_mgr = tcp_mgr,
          .firewall = fw,
          .agent = agent,
          .registry = registry,
          .start_time = start_time,
          .rate_state = {}
      }))
{
    _impl->rate_state.prev_time = start_time;
}

TelemetryAPI::~TelemetryAPI() = default;

// ════════════════════════════════════════════
// 速率计算
// ════════════════════════════════════════════

void TelemetryAPI::update_rates() {
    auto& rs = _impl->rate_state;
    auto& gm = _impl->global_metrics;
    auto now = std::chrono::steady_clock::now();

    uint64_t cur_pkts_rx = gm.packets_rx.load(std::memory_order_relaxed);
    uint64_t cur_pkts_tx = gm.packets_tx.load(std::memory_order_relaxed);
    uint64_t cur_bytes_rx = gm.bytes_rx.load(std::memory_order_relaxed);
    uint64_t cur_bytes_tx = gm.bytes_tx.load(std::memory_order_relaxed);

    if (!rs.initialized) {
        // 首次调用：只记录基线，速率保持 0
        rs.prev_packets_rx = cur_pkts_rx;
        rs.prev_packets_tx = cur_pkts_tx;
        rs.prev_bytes_rx = cur_bytes_rx;
        rs.prev_bytes_tx = cur_bytes_tx;
        rs.prev_time = now;
        rs.initialized = true;
        return;
    }

    double dt = std::chrono::duration<double>(now - rs.prev_time).count();
    if (dt < 0.001) return;  // 避免除零，至少间隔 1ms

    // 计算瞬时速率
    double inst_pps_rx = static_cast<double>(cur_pkts_rx - rs.prev_packets_rx) / dt;
    double inst_pps_tx = static_cast<double>(cur_pkts_tx - rs.prev_packets_tx) / dt;
    double inst_bps_rx = static_cast<double>(cur_bytes_rx - rs.prev_bytes_rx) / dt;
    double inst_bps_tx = static_cast<double>(cur_bytes_tx - rs.prev_bytes_tx) / dt;

    // 指数移动平均 (EMA) 平滑，α = 0.3
    // 避免单次采样波动过大
    constexpr double alpha = 0.3;
    rs.pps_rx = alpha * inst_pps_rx + (1.0 - alpha) * rs.pps_rx;
    rs.pps_tx = alpha * inst_pps_tx + (1.0 - alpha) * rs.pps_tx;
    rs.bps_rx = alpha * inst_bps_rx + (1.0 - alpha) * rs.bps_rx;
    rs.bps_tx = alpha * inst_bps_tx + (1.0 - alpha) * rs.bps_tx;

    // 更新基线
    rs.prev_packets_rx = cur_pkts_rx;
    rs.prev_packets_tx = cur_pkts_tx;
    rs.prev_bytes_rx = cur_bytes_rx;
    rs.prev_bytes_tx = cur_bytes_tx;
    rs.prev_time = now;
}

// ════════════════════════════════════════════
// RTT Percentile 计算 (从 Histogram 快照)
// ════════════════════════════════════════════

/**
 * 线性插值估算分位数
 *
 * 算法: 与 Prometheus histogram_quantile() 相同
 * 假设桶内观测值均匀分布
 *
 * 1. 计算目标排名: rank = q × count
 * 2. 找到 rank 落入的累积桶 i
 * 3. 在桶 [lower, upper] 内线性插值:
 *    result = lower + (rank - prev_count) / (curr_count - prev_count) × (upper - lower)
 */
static double percentile_from_histogram(const Histogram::Snapshot& snap, double q) {
    if (snap.count == 0) return 0.0;

    double rank = q * static_cast<double>(snap.count);

    for (size_t i = 0; i < snap.bucket_counts.size(); ++i) {
        double curr = static_cast<double>(snap.bucket_counts[i]);
        if (curr >= rank) {
            double prev = (i == 0) ? 0.0
                : static_cast<double>(snap.bucket_counts[i - 1]);
            double lower = (i == 0) ? 0.0 : snap.boundaries[i - 1];
            double upper = (i < snap.boundaries.size())
                ? snap.boundaries[i]
                : (snap.boundaries.empty() ? 1.0 : snap.boundaries.back() * 2.0);

            if (curr == prev) return upper;

            double fraction = (rank - prev) / (curr - prev);
            return lower + fraction * (upper - lower);
        }
    }
    return snap.boundaries.empty() ? 0.0 : snap.boundaries.back();
}

// ════════════════════════════════════════════
// 子集查询
// ════════════════════════════════════════════

StackStatus::Traffic TelemetryAPI::traffic() {
    update_rates();

    auto& gm = _impl->global_metrics;
    auto& rs = _impl->rate_state;

    return StackStatus::Traffic{
        .packets_rx = gm.packets_rx.load(std::memory_order_relaxed),
        .packets_tx = gm.packets_tx.load(std::memory_order_relaxed),
        .bytes_rx   = gm.bytes_rx.load(std::memory_order_relaxed),
        .bytes_tx   = gm.bytes_tx.load(std::memory_order_relaxed),
        .pps_rx     = rs.pps_rx,
        .pps_tx     = rs.pps_tx,
        .bps_rx     = rs.bps_rx,
        .bps_tx     = rs.bps_tx,
    };
}

StackStatus::TCP TelemetryAPI::tcp_stats() {
    auto& gm = _impl->global_metrics;

    StackStatus::TCP tcp{};
    tcp.active_connections = gm.active_connections.load(std::memory_order_relaxed);
    tcp.total_established = gm.conn_established.load(std::memory_order_relaxed);
    tcp.total_reset = gm.conn_reset.load(std::memory_order_relaxed);
    tcp.total_timeout = gm.conn_timeout.load(std::memory_order_relaxed);
    tcp.total_retransmits = gm.total_retransmits.load(std::memory_order_relaxed);

    // RTT 从 Histogram 计算
    auto* rtt_hist = _impl->registry.find_histogram("neustack_tcp_rtt_us");
    if (rtt_hist) {
        auto snap = rtt_hist->snapshot();
        tcp.rtt.samples = snap.count;
        if (snap.count > 0) {
            tcp.rtt.avg_us = snap.sum / static_cast<double>(snap.count);
            tcp.rtt.p50_us = percentile_from_histogram(snap, 0.50);
            tcp.rtt.p90_us = percentile_from_histogram(snap, 0.90);
            tcp.rtt.p99_us = percentile_from_histogram(snap, 0.99);
            // min 近似: 第一个非空桶的下界
            tcp.rtt.min_us = percentile_from_histogram(snap, 0.0);
            // max 近似: P99.9 或最后桶上界
            tcp.rtt.max_us = percentile_from_histogram(snap, 0.999);
        }
    }

    // 平均 cwnd: 遍历所有连接
    double cwnd_sum = 0;
    uint32_t conn_count = 0;
    _impl->tcp_mgr.for_each_connection([&](const neustack::TCB& tcb) {
        cwnd_sum += tcb.congestion_control ? tcb.congestion_control->cwnd() : 0;
        ++conn_count;
    });
    tcp.avg_cwnd = (conn_count > 0) ? (cwnd_sum / conn_count) : 0.0;

    return tcp;
}

StackStatus::Security TelemetryAPI::security_stats() {
    StackStatus::Security sec{};

    sec.firewall_enabled = (_impl->firewall != nullptr);
    sec.ai_enabled = (_impl->agent != nullptr);

    if (_impl->firewall) {
        sec.shadow_mode = _impl->firewall->shadow_mode();
        sec.packets_dropped = _impl->firewall->stats().packets_dropped;
        sec.packets_alerted = _impl->firewall->stats().packets_alerted;
    }

    if (_impl->security_metrics) {
        auto snap = _impl->security_metrics->snapshot();
        sec.pps = snap.pps;
        sec.syn_rate = snap.syn_rate;
        sec.syn_synack_ratio = snap.syn_to_synack_ratio;
        sec.rst_ratio = snap.rst_ratio;
    }

    if (_impl->agent) {
        sec.anomaly_score = _impl->agent->anomaly_score();
        sec.agent_state = neustack::agent_state_name(_impl->agent->state());
        sec.predicted_bandwidth_bps = static_cast<float>(_impl->agent->predicted_bw());
    } else {
        sec.anomaly_score = 0.0f;
        sec.agent_state = "DISABLED";
        sec.predicted_bandwidth_bps = 0.0f;
    }

    return sec;
}

// ════════════════════════════════════════════
// 完整状态快照
// ════════════════════════════════════════════

StackStatus TelemetryAPI::status() {
    StackStatus s{};

    auto now = std::chrono::steady_clock::now();
    s.timestamp = now;
    s.uptime_seconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            now - _impl->start_time).count());

    s.traffic = traffic();
    s.tcp = tcp_stats();
    s.security = security_stats();

    // AI 面
    // NetworkAgent 没有 orca_loaded/anomaly_loaded/bandwidth_loaded 方法
    // 简化: 如果 agent 存在，说明 AI 模块已启用
    if (_impl->agent) {
        s.ai.enabled = true;
        s.ai.orca_status = "loaded";
        s.ai.anomaly_status = "loaded";
        s.ai.bandwidth_status = "loaded";
        s.ai.current_alpha = _impl->agent->current_alpha();
    } else {
        s.ai.enabled = false;
        s.ai.orca_status = "disabled";
        s.ai.anomaly_status = "disabled";
        s.ai.bandwidth_status = "disabled";
        s.ai.current_alpha = 0.0f;
    }

    return s;
}

// ════════════════════════════════════════════
// 导出快捷方法
// ════════════════════════════════════════════

std::string TelemetryAPI::to_json(bool pretty) {
    JsonExporter exporter(pretty);
    return exporter.serialize(_impl->registry);
}

std::string TelemetryAPI::to_prometheus() {
    PrometheusExporter exporter;
    return exporter.serialize(_impl->registry);
}

std::vector<ConnectionDetail> TelemetryAPI::connections() {
    std::vector<ConnectionDetail> result;
    // 预分配: 用 connection_count 估算 (有轻微 TOCTOU，但只影响 reserve)
    result.reserve(_impl->tcp_mgr.connection_count());

    _impl->tcp_mgr.for_each_connection([&](const neustack::TCB& tcb) {
        ConnectionDetail d;
        d.local_ip = tcb.t_tuple.local_ip;
        d.local_port = tcb.t_tuple.local_port;
        d.remote_ip = tcb.t_tuple.remote_ip;
        d.remote_port = tcb.t_tuple.remote_port;
        d.state = neustack::tcp_state_name(tcb.state);
        d.rtt_us = tcb.last_rtt_us;
        d.srtt_us = tcb.srtt_us;
        d.cwnd = tcb.congestion_control ? tcb.congestion_control->cwnd() : 0;
        d.bytes_in_flight = (tcb.snd_nxt >= tcb.snd_una)
            ? (tcb.snd_nxt - tcb.snd_una) : 0;
        d.send_buffer_used = static_cast<uint32_t>(tcb.send_buffer.size());
        d.recv_buffer_used = static_cast<uint32_t>(tcb.recv_buffer.size());
        d.bytes_sent = 0;        // TCB 暂无此字段
        d.bytes_received = 0;    // TCB 暂无此字段
        d.established_at = tcb.last_activity; // 近似: 使用 last_activity
        result.push_back(std::move(d));
    });

    return result;
}

std::vector<ConnectionDetail> TelemetryAPI::connections_by_ip(uint32_t remote_ip) {
    auto all = connections();
    std::erase_if(all, [remote_ip](const auto& c) {
        return c.remote_ip != remote_ip;
    });
    return all;
}

std::vector<ConnectionDetail> TelemetryAPI::connections_by_port(uint16_t local_port) {
    auto all = connections();
    std::erase_if(all, [local_port](const auto& c) {
        return c.local_port != local_port;
    });
    return all;
}
