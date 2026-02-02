#ifndef NEUSTACK_METRICS_AI_FEATURES_HPP
#define NEUSTACK_METRICS_AI_FEATURES_HPP

#include "neustack/metrics/tcp_sample.hpp"
#include "neustack/metrics/global_metrics.hpp"
#include <vector>

namespace neustack {

// Orca 特征（6维）
struct  OrcaFeatures {
    float throughput_normalized;    // 归一化吞吐量：吞吐量 / 估计带宽
    float queuing_delay_normalized; // 归一化排队延迟：排队延迟 / min_RTT
    float rtt_ratio;                // RTT 比率：RTT / min_RTT
    float loss_rate;                // 丢包率： [0, 1]
    float cwnd_normalized;          // 归一化拥塞窗口：cwnd / BDP
    float in_flight_ratio;          // 在途包比例：in_flight / cwnd

    static OrcaFeatures from_sample(const TCPSample &s, uint32_t est_bw);
    std::vector<float> to_vector() const;
    static constexpr size_t dim() { return 6; }
};

// 异常检测特征（5维）
struct AnomalyFeatures {
    float syn_rate;         // SYN/s
    float rst_rate;         // RST/s
    float new_conn_rate;    // 新连接/s
    float packet_rate;      // 包/s
    float avg_packet_size;  // 平均包大小

    static AnomalyFeatures from_delta(
        const GlobalMetrics::Snapshot::Delta &delta,
        double interval_sec
    );
    std::vector<float> to_vector() const;
    static constexpr size_t dim() { return 5; }
};

// 带宽预测特征（时序输入，过去 N 个采样周期的历史数据，组成 LSTM 的输入序列）
struct BandwidthFeatures {
    std::vector<float> throughput_history;  // 历史吞吐量（归一化）
    std::vector<float> rtt_history;         // 历史 RTT 比率
    std::vector<float> loss_history;        // 历史丢包率

    static BandwidthFeatures from_samples(
        const std::vector<TCPSample> &samples,
        uint32_t est_bw = 0
    );
    std::vector<float> to_vector() const; // 展平: [throughput..., rtt..., loss...]
};

} // namespace neustack



#endif