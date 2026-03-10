#ifndef NEUSTACK_METRICS_AI_FEATURES_HPP
#define NEUSTACK_METRICS_AI_FEATURES_HPP

#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include <vector>

namespace neustack {

// Orca 特征（7维，匹配 Python 训练归一化）
struct OrcaFeatures {
    float throughput_normalized;       // delivery_rate / est_bw, clip[0,2]
    float queuing_delay_normalized;    // (rtt - min_rtt) / min_rtt, clip[0,5]
    float rtt_ratio;                   // rtt / min_rtt, clip[1,5]
    float loss_rate;                   // packets_lost / packets_sent, clip[0,1]
    float cwnd_normalized;             // cwnd / BDP, clip[0,10] where BDP=est_bw*min_rtt/1e6/MSS
    float in_flight_ratio;             // bytes_in_flight / (cwnd*MSS), clip[0,2]
    float predicted_bw_normalized;     // predicted_bw / MAX_BW(10MB/s), clip[0,2]

    static OrcaFeatures from_sample(const TCPSample &s, uint32_t est_bw, float predicted_bw);
    std::vector<float> to_vector() const;
    static constexpr size_t dim() { return 7; }
};

// 异常检测特征（8维，ratio-based volume-invariant features）
struct AnomalyFeatures {
    float log_pkt_rate;      // log1p(pkt_rx+pkt_tx) / log1p(40000), clip[0,1]
    float bytes_per_pkt;     // bytes_tx / max(pkt_tx,1) / 1500, clip[0,1]
    float syn_ratio;         // syn_received / max(pkt_rx,1), clip[0,1]
    float rst_ratio;         // rst_received / max(pkt_rx,1), clip[0,1]
    float conn_completion;   // conn_est / max(syn,1), clip[0,1]; syn=0 → 1.0
    float tx_rx_ratio;       // pkt_tx / max(pkt_rx,1) / 2, clip[0,1]
    float log_active_conn;   // log1p(active_conn) / log1p(1000), clip[0,1]
    float log_conn_reset;    // log1p(conn_reset) / log1p(100), clip[0,1]

    static AnomalyFeatures from_delta(
        const GlobalMetrics::Snapshot::Delta &delta,
        uint32_t active_connections
    );
    std::vector<float> to_vector() const;
    static constexpr size_t dim() { return 8; }
};

// 带宽预测特征（时序输入，过去 N 个采样周期的历史数据，组成 LSTM 的输入序列）
struct BandwidthFeatures {
    std::vector<float> throughput_history;  // 历史吞吐量（归一化）
    std::vector<float> rtt_history;         // 历史 RTT 比率
    std::vector<float> loss_history;        // 历史丢包率

    static BandwidthFeatures from_samples(
        const std::vector<TCPSample> &samples,
        uint32_t min_rtt_us = 0
    );
    std::vector<float> to_vector() const; // 展平: [throughput..., rtt..., loss...]
};

} // namespace neustack



#endif