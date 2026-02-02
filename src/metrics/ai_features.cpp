#include "neustack/metrics/ai_features.hpp"
#include <cmath>

namespace neustack {

// ============================================================================
// OrcaFeatures - 拥塞控制特征
// ============================================================================

OrcaFeatures OrcaFeatures::from_sample(const TCPSample& s, uint32_t est_bw) {
    OrcaFeatures f{};

    // BDP = bandwidth * min_rtt (bytes)
    // est_bw: bytes/s, min_rtt_us: 微秒
    uint32_t bdp = (est_bw > 0 && s.min_rtt_us > 0)
        ? static_cast<uint32_t>(
            static_cast<uint64_t>(est_bw) * s.min_rtt_us / 1000000
          )
        : 0;

    constexpr uint32_t MSS = 1460;

    // 归一化吞吐量
    f.throughput_normalized = (est_bw > 0)
        ? static_cast<float>(s.delivery_rate) / est_bw
        : 0.0f;

    // 归一化排队延迟
    f.queuing_delay_normalized = (s.min_rtt_us > 0)
        ? static_cast<float>(s.queuing_delay_us()) / s.min_rtt_us
        : 0.0f;

    // RTT 比值
    f.rtt_ratio = s.rtt_ratio();

    // 丢包率 (使用真实统计)
    f.loss_rate = s.loss_rate();

    // 归一化拥塞窗口 (cwnd * MSS / BDP)
    f.cwnd_normalized = (bdp > 0)
        ? static_cast<float>(s.cwnd * MSS) / bdp
        : 1.0f;

    // 在途数据比例 (bytes_in_flight / cwnd_bytes)
    uint32_t cwnd_bytes = s.cwnd * MSS;
    f.in_flight_ratio = (cwnd_bytes > 0)
        ? static_cast<float>(s.bytes_in_flight) / cwnd_bytes
        : 0.0f;

    return f;
}

std::vector<float> OrcaFeatures::to_vector() const {
    return {
        throughput_normalized,
        queuing_delay_normalized,
        rtt_ratio,
        loss_rate,
        cwnd_normalized,
        in_flight_ratio
    };
}

// ============================================================================
// AnomalyFeatures - 异常检测特征
// ============================================================================

AnomalyFeatures AnomalyFeatures::from_delta(
    const GlobalMetrics::Snapshot::Delta& delta,
    double interval_sec
) {
    AnomalyFeatures f{};

    if (interval_sec <= 0) interval_sec = 1.0;

    f.syn_rate = static_cast<float>(delta.syn_received / interval_sec);
    f.rst_rate = static_cast<float>(delta.rst_received / interval_sec);
    f.new_conn_rate = static_cast<float>(delta.conn_established / interval_sec);
    f.packet_rate = static_cast<float>(delta.packets_rx / interval_sec);
    f.avg_packet_size = (delta.packets_rx > 0)
        ? static_cast<float>(delta.bytes_rx) / delta.packets_rx
        : 0.0f;

    return f;
}

std::vector<float> AnomalyFeatures::to_vector() const {
    return {
        syn_rate,
        rst_rate,
        new_conn_rate,
        packet_rate,
        avg_packet_size
    };
}

// ============================================================================
// BandwidthFeatures - 带宽预测特征
// ============================================================================

BandwidthFeatures BandwidthFeatures::from_samples(
    const std::vector<TCPSample>& samples,
    uint32_t est_bw
) {
    BandwidthFeatures f;
    f.throughput_history.reserve(samples.size());
    f.rtt_history.reserve(samples.size());
    f.loss_history.reserve(samples.size());

    for (const auto& s : samples) {
        // 吞吐量归一化
        float tp = (est_bw > 0)
            ? static_cast<float>(s.delivery_rate) / est_bw
            : static_cast<float>(s.delivery_rate) / 1e6f;  // 默认按 MB/s 归一化
        f.throughput_history.push_back(tp);

        // RTT 比值
        f.rtt_history.push_back(s.rtt_ratio());

        // 丢包率 (使用真实统计)
        f.loss_history.push_back(s.loss_rate());
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
