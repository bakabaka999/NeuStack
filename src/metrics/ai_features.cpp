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

    // Ratio-based volume-invariant features (matching Python csv_to_dataset.py)
    float pkt_rx = static_cast<float>(delta.packets_rx);
    float pkt_tx = static_cast<float>(delta.packets_tx);
    float bytes_tx = static_cast<float>(delta.bytes_tx);
    float syn = static_cast<float>(delta.syn_received);
    float rst = static_cast<float>(delta.rst_received);
    float conn_est = static_cast<float>(delta.conn_established);
    float conn_rst = static_cast<float>(delta.conn_reset);
    float active = static_cast<float>(active_connections);

    // 0. log_pkt_rate: log-compressed total packet rate
    f.log_pkt_rate = std::clamp(std::log1p(pkt_rx + pkt_tx) / std::log1p(40000.0f), 0.0f, 1.0f);

    // 1. bytes_per_pkt: average packet size / 1500 (MTU)
    f.bytes_per_pkt = std::clamp(bytes_tx / std::max(pkt_tx, 1.0f) / 1500.0f, 0.0f, 1.0f);

    // 2. syn_ratio: SYN fraction of received packets — key attack signal
    f.syn_ratio = std::clamp(syn / std::max(pkt_rx, 1.0f), 0.0f, 1.0f);

    // 3. rst_ratio: RST fraction of received packets — port scan signal
    f.rst_ratio = std::clamp(rst / std::max(pkt_rx, 1.0f), 0.0f, 1.0f);

    // 4. conn_completion: SYN→established completion rate; syn=0 means no SYN traffic → 1.0
    f.conn_completion = (syn == 0.0f)
        ? 1.0f
        : std::clamp(conn_est / syn, 0.0f, 1.0f);

    // 5. tx_rx_ratio: directional asymmetry, /2 so normal ~0.5
    f.tx_rx_ratio = std::clamp(pkt_tx / std::max(pkt_rx, 1.0f) / 2.0f, 0.0f, 1.0f);

    // 6. log_active_conn: log-compressed connection count
    f.log_active_conn = std::clamp(std::log1p(active) / std::log1p(1000.0f), 0.0f, 1.0f);

    // 7. log_conn_reset: log-compressed reset count (absolute, not ratio)
    // Avoids ratio instability when conn_established is small (1-2 connections)
    f.log_conn_reset = std::clamp(std::log1p(conn_rst) / std::log1p(100.0f), 0.0f, 1.0f);

    return f;
}

std::vector<float> AnomalyFeatures::to_vector() const {
    return {
        log_pkt_rate,
        bytes_per_pkt,
        syn_ratio,
        rst_ratio,
        conn_completion,
        tx_rx_ratio,
        log_active_conn,
        log_conn_reset
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
