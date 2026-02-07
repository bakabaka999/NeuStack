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

// 异常检测特征（8维，匹配 Python 训练归一化）
struct AnomalyFeatures {
    float packets_rx_norm;       // packets_rx / 20000, clip[0,1]
    float packets_tx_norm;       // packets_tx / 20000, clip[0,1]
    float bytes_tx_norm;         // bytes_tx / 30000000, clip[0,1]
    float syn_rate_norm;         // syn_received / 100, clip[0,1]
    float rst_rate_norm;         // rst_received / 100, clip[0,1]
    float conn_established_norm; // conn_established / 100, clip[0,1]
    float tx_rx_ratio_norm;      // (packets_tx/packets_rx) / 10, clip[0,1]
    float active_conn_norm;      // active_connections / 100, clip[0,1]

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