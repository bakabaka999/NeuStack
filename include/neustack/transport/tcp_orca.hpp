#ifndef NEUSTACK_TRANSPORT_TCP_ORCA_HPP
#define NEUSTACK_TRANSPORT_TCP_ORCA_HPP

#include "neustack/transport/tcp_cubic.hpp"
#include <cmath>
#include <functional>

namespace neustack {
    
/**
 * Orca 拥塞控制
 *
 * 双层架构:
 * - Layer 1: CUBIC 计算基础 cwnd
 * - Layer 2: AI 模型输出 α ∈ [-1, 1] 调制
 *
 * 最终 cwnd = 2^α × cwnd_cubic
 *
 * 参考: Orca: A Differential Congestion Control System (NSDI 2022)
 */
class TCPOrca : public ICongestionControl {
public:
    // α 的回调类型：输入当前状态，返回 α ∈ [-1, 1]
    // 状态: (throughput, rtt, min_rtt, loss_rate, cwnd, in_flight, predicted_bw)
    using AlphaCallback = std::function<float(
        float throughput_normalized,
        float queuing_delay_normalized,
        float rtt_ratio,
        float loss_rate,
        float cwnd_normalized,
        float in_flight_ratio,
        float predicted_bw_normalized)>;

    static constexpr uint32_t MAX_CWND = 65535 * 16;
    static constexpr uint32_t MIN_CWND_MSS = 2;

    explicit TCPOrca(
        uint32_t mss = 1460,
        AlphaCallback alpha_cb = nullptr
    )
        : _cubic(mss)
        , _mss(mss)
        , _alpha_callback(std::move(alpha_cb))
        , _current_alpha(0.0f)
        , _last_throughput(0)
        , _min_rtt_us(UINT32_MAX)
        , _bytes_in_flight(0)
        , _predicted_bw(0)
    {}

    void on_ack(uint32_t bytes_acked, uint32_t rtt_us) override {
        _cubic.on_ack(bytes_acked, rtt_us);

        // 更新 RTT 估计
        if (rtt_us > 0 && rtt_us < _min_rtt_us) {
            _min_rtt_us = rtt_us;
        }
        _last_rtt_us = rtt_us;

        // 更新 in-flight
        if (_bytes_in_flight >= bytes_acked) {
            _bytes_in_flight -= bytes_acked;
        }

        // 如果有 AI 回调，获取alpha
        if (_alpha_callback) {
            _current_alpha = compute_alpha();
        }
        // 否则 α 保持上次设置的值 (通过 set_alpha)
    }

    void on_loss(uint32_t bytes_lost) override {
        _cubic.on_loss(bytes_lost);
        _loss_count++;
    }

    void on_timeout() {
        _cubic.on_timeout();
    }

    uint32_t cwnd() const override {
        // 应用 α 调制
        uint32_t cwnd_cubic = _cubic.cwnd();
        float multiplier = std::pow(2.0f, _current_alpha);
        uint32_t cwnd_orca = static_cast<uint32_t>(cwnd_cubic * multiplier);

        // 限制范围
        return std::clamp(cwnd_orca, MIN_CWND_MSS * _mss, MAX_CWND);
    }

    uint32_t ssthresh() const override {
        return _cubic.ssthresh();
    }

    // ─── Orca 特有接口 ───

    // 设置 alpha
    void set_alpha(float alpha) {
        _current_alpha = std::clamp(alpha, -1.0f, 1.0f);
    }

    float alpha() const { return _current_alpha; }

    // 获取 CUBIC 的基础 cwnd
    uint32_t cwnd_cubic() const { return _cubic.cwnd(); }

    // 设置带宽预测值 (供 IntelligencePlane 调用)
    void set_predicted_bandwidth(uint64_t bw) {
        _predicted_bw = bw;
    }

    // 设置实际吞吐量 (供协议栈在收到 ACK 时调用)
    void set_delivery_rate(uint64_t rate) {
        _last_throughput = rate;
    }

    // 记录发送
    void on_send(uint32_t bytes) {
        _bytes_in_flight += bytes;
        _packets_sent++;
    }

    // 重置采样周期统计
    void reset_period_stats() {
        _loss_count = 0;
        _packets_sent = 0;
    }

    // ─── 状态获取（供 AI 采样）───

    uint64_t throughput() const { return _last_throughput; }
    uint32_t rtt_us() const { return _last_rtt_us; }
    uint32_t min_rtt_us() const { return _min_rtt_us; }
    uint32_t bytes_in_flight() const { return _bytes_in_flight; }
    uint64_t predicted_bw() const { return _predicted_bw; }

    float loss_rate() const {
        return _packets_sent > 0
            ? static_cast<float>(_loss_count) / _packets_sent
            : 0.0f;
    }

private:
    float compute_alpha() {
        // 归一化参数
        constexpr float MAX_THROUGHPUT = 100e6f; // 100 MB/s
        constexpr float MAX_CWND = 65535.0f;

        float throughput_norm = std::min(_last_throughput / MAX_THROUGHPUT, 1.0f);
        float queuing_delay = (_min_rtt_us > 0 && _last_rtt_us > _min_rtt_us)
            ? static_cast<float>(_last_rtt_us - _min_rtt_us) / _min_rtt_us
            : 0.0f;
        float rtt_ratio = (_min_rtt_us > 0)
            ? static_cast<float>(_last_rtt_us) / _min_rtt_us
            : 1.0f;
        float cwnd_norm = static_cast<float>(_cubic.cwnd()) / MAX_CWND;
        float in_flight_ratio = (_cubic.cwnd() > 0)
            ? static_cast<float>(_bytes_in_flight) / (_cubic.cwnd())
            : 0.0f;
        float predicted_bw_norm = std::min(_predicted_bw / MAX_THROUGHPUT, 1.0f);

        return _alpha_callback(
            throughput_norm,
            queuing_delay,
            rtt_ratio,
            loss_rate(),
            cwnd_norm,
            in_flight_ratio,
            predicted_bw_norm
        );
    }

    TCPCubic _cubic;
    uint32_t _mss;

    // AI 回调
    AlphaCallback _alpha_callback;
    float _current_alpha;

    // 状态
    uint64_t _last_throughput;
    uint32_t _last_rtt_us = 0;
    uint32_t _min_rtt_us;
    uint32_t _bytes_in_flight;
    uint64_t _predicted_bw;

    // 采样周期统计
    uint32_t _loss_count = 0;
    uint32_t _packets_sent = 0;
};

} // namespace neustack


#endif // NEUSTACK_TRANSPORT_TCP_ORCA_HPP
