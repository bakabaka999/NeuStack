#include "neustack/metrics/ai_features.hpp"
#include <algorithm>
#include <cmath>

namespace neustack {

// ============================================================================
// OrcaFeatures - 拥塞控制特征 (7维，匹配 Python 训练归一化)
// ============================================================================

OrcaFeatures OrcaFeatures::from_sample(const TCPSample& s, uint32_t est_bw, float predicted_bw) {
    OrcaFeatures f{};

    constexpr uint32_t MSS = 1460;
    constexpr float MAX_BW = 10e6f;  // 10 MB/s, matches Python MAX_BW

    // BDP = est_bw * min_rtt_us / 1e6 (bytes), then / MSS for packets
    float bdp_packets = (est_bw > 0 && s.min_rtt_us > 0)
        ? static_cast<float>(static_cast<uint64_t>(est_bw) * s.min_rtt_us) / 1e6f / MSS
        : 0.0f;

    // throughput_norm = delivery_rate / est_bw, clip[0,2]
    f.throughput_normalized = (est_bw > 0)
        ? std::clamp(static_cast<float>(s.delivery_rate) / est_bw, 0.0f, 2.0f)
        : 0.0f;

    // queuing_delay_norm = (rtt - min_rtt) / min_rtt, clip[0,5]
    f.queuing_delay_normalized = (s.min_rtt_us > 0)
        ? std::clamp(static_cast<float>(s.queuing_delay_us()) / s.min_rtt_us, 0.0f, 5.0f)
        : 0.0f;

    // rtt_ratio = rtt / min_rtt, clip[1,5]
    f.rtt_ratio = std::clamp(s.rtt_ratio(), 1.0f, 5.0f);

    // loss_rate = packets_lost / packets_sent, clip[0,1]
    f.loss_rate = std::clamp(s.loss_rate(), 0.0f, 1.0f);

    // cwnd_norm = cwnd / BDP (both in packets), clip[0,10]
    f.cwnd_normalized = (bdp_packets > 0.0f)
        ? std::clamp(static_cast<float>(s.cwnd) / bdp_packets, 0.0f, 10.0f)
        : 1.0f;

    // in_flight_ratio = bytes_in_flight / (cwnd * MSS), clip[0,2]
    uint32_t cwnd_bytes = s.cwnd * MSS;
    f.in_flight_ratio = (cwnd_bytes > 0)
        ? std::clamp(static_cast<float>(s.bytes_in_flight) / cwnd_bytes, 0.0f, 2.0f)
        : 0.0f;

    // predicted_bw_normalized = predicted_bw / MAX_BW, clip[0,2]
    f.predicted_bw_normalized = std::clamp(predicted_bw / MAX_BW, 0.0f, 2.0f);

    return f;
}

std::vector<float> OrcaFeatures::to_vector() const {
    return {
        throughput_normalized,
        queuing_delay_normalized,
        rtt_ratio,
        loss_rate,
        cwnd_normalized,
        in_flight_ratio,
        predicted_bw_normalized
    };
}

// ============================================================================
// AnomalyFeatures - 异常检测特征 (8维，匹配 Python 训练归一化)
// ============================================================================

AnomalyFeatures AnomalyFeatures::from_delta(
    const GlobalMetrics::Snapshot::Delta& delta,
    uint32_t active_connections
) {
    AnomalyFeatures f{};

    // All features normalized to [0,1] matching Python training (csv_to_dataset.py)
    f.packets_rx_norm = std::clamp(static_cast<float>(delta.packets_rx) / 20000.0f, 0.0f, 1.0f);
    f.packets_tx_norm = std::clamp(static_cast<float>(delta.packets_tx) / 20000.0f, 0.0f, 1.0f);
    f.bytes_tx_norm = std::clamp(static_cast<float>(delta.bytes_tx) / 30000000.0f, 0.0f, 1.0f);
    f.syn_rate_norm = std::clamp(static_cast<float>(delta.syn_received) / 100.0f, 0.0f, 1.0f);
    f.rst_rate_norm = std::clamp(static_cast<float>(delta.rst_received) / 100.0f, 0.0f, 1.0f);
    f.conn_established_norm = std::clamp(static_cast<float>(delta.conn_established) / 100.0f, 0.0f, 1.0f);

    // tx/rx ratio
    float tx_rx_ratio = (delta.packets_rx > 0)
        ? static_cast<float>(delta.packets_tx) / static_cast<float>(delta.packets_rx)
        : 0.0f;
    f.tx_rx_ratio_norm = std::clamp(tx_rx_ratio / 10.0f, 0.0f, 1.0f);

    f.active_conn_norm = std::clamp(static_cast<float>(active_connections) / 100.0f, 0.0f, 1.0f);

    return f;
}

std::vector<float> AnomalyFeatures::to_vector() const {
    return {
        packets_rx_norm,
        packets_tx_norm,
        bytes_tx_norm,
        syn_rate_norm,
        rst_rate_norm,
        conn_established_norm,
        tx_rx_ratio_norm,
        active_conn_norm
    };
}

// ============================================================================
// BandwidthFeatures - 带宽预测特征
// ============================================================================

BandwidthFeatures BandwidthFeatures::from_samples(
    const std::vector<TCPSample>& samples,
    uint32_t min_rtt_us
) {
    BandwidthFeatures f;
    f.throughput_history.reserve(samples.size());
    f.rtt_history.reserve(samples.size());
    f.loss_history.reserve(samples.size());

    constexpr float MAX_BW = 10e6f;  // 10 MB/s, matches Python MAX_BW

    for (const auto& s : samples) {
        // throughput = delivery_rate / MAX_BW, clip[0,1]
        f.throughput_history.push_back(
            std::clamp(static_cast<float>(s.delivery_rate) / MAX_BW, 0.0f, 1.0f)
        );

        // rtt_ratio = rtt / min_rtt, clip[1,5]
        float rtt_ratio;
        if (min_rtt_us > 0) {
            rtt_ratio = static_cast<float>(s.rtt_us) / min_rtt_us;
        } else if (s.min_rtt_us > 0) {
            rtt_ratio = s.rtt_ratio();
        } else {
            rtt_ratio = 1.0f;
        }
        f.rtt_history.push_back(std::clamp(rtt_ratio, 1.0f, 5.0f));

        // loss_rate, clip[0,1]
        f.loss_history.push_back(std::clamp(s.loss_rate(), 0.0f, 1.0f));
    }

    return f;
}

std::vector<float> BandwidthFeatures::to_vector() const {
    std::vector<float> v;
    v.reserve(throughput_history.size() + rtt_history.size() + loss_history.size());

    // 展平: [throughput_0, ..., throughput_n, rtt_0, ..., rtt_n, loss_0, ..., loss_n]
    v.insert(v.end(), throughput_history.begin(), throughput_history.end());
    v.insert(v.end(), rtt_history.begin(), rtt_history.end());
    v.insert(v.end(), loss_history.begin(), loss_history.end());

    return v;
}

} // namespace neustack
