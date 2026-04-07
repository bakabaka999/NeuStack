#ifndef NEUSTACK_TRANSPORT_TCP_CUBIC_HPP
#define NEUSTACK_TRANSPORT_TCP_CUBIC_HPP

#include "neustack/transport/tcp_tcb.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>

namespace neustack {
    
/**
 * CUBIC 拥塞控制 (RFC 8312)
 *
 * 特点:
 * - 使用三次函数而非线性增长
 * - 对高带宽长延迟网络（BDP 大）更友好
 * - Linux 默认拥塞控制算法
 */
class TCPCubic : public ICongestionControl {
public:
    // CUBIC 常数
    static constexpr double C = 0.4;        // 缩放常数
    static constexpr double BETA = 0.7;     // 乘法减少因子
    static constexpr uint32_t MIN_CWND = 2; // 最小 cwnd（MSS单位）

    explicit TCPCubic(uint32_t mss = 1460)
        : _mss(mss < 536 ? 536 : mss)
        , _cwnd(10 * mss)              // 初始 10 MSS (RFC 6928)
        , _ssthresh(65535)
        , _w_max(0)
        , _k(0)
        , _epoch_start()
        , _in_slow_start(true)
        , _tcp_friendliness(true)
        , _w_tcp(0)
    {}

    void on_ack(uint32_t bytes_acked, uint32_t rtt_us) override {
        _last_rtt_us = rtt_us;

        if(_in_slow_start && _cwnd < _ssthresh) {
            // 慢启动，指数增长
            _cwnd += bytes_acked;
            return;
        }

        _in_slow_start = false;

        // 拥塞避免：CUBIC 增长
        auto now = std::chrono::steady_clock::now();

        // 如果是首次进入拥塞避免，记录 epoch 开始时间
        if (_epoch_start.time_since_epoch().count() == 0) {
            _epoch_start = now;
            _w_tcp = _cwnd; // TCP Reno 友好模式起点
        }

        // 计算自 epoch 开始的时间 (秒)
        double t = std::chrono::duration<double>(now - _epoch_start).count();

        // CUBIC 窗口目标
        double w_cubic = cubic_window(t);

        // TCP 友好模式（确保不比 Reno 差）
        if (_tcp_friendliness) {
            // Reno 线性增长: W_tcp(t) = W_max * (1-β) + 3*β/(2-β) * t/RTT
            double rtt_sec = _last_rtt_us / 1e6;
            if (rtt_sec > 0) {
                _w_tcp += (3 * BETA / (2 - BETA)) * (t / rtt_sec) * _mss;
            }
            w_cubic = std::max(w_cubic, _w_tcp);
        }

        // 更新 cwnd — clamp 到 uint32_t 范围，避免 UB
        w_cubic = std::min(w_cubic, static_cast<double>(UINT32_MAX));
        w_cubic = std::max(w_cubic, 0.0);
        uint32_t target = static_cast<uint32_t>(w_cubic);
        if (target > _cwnd) {
            // 增长：每个 ACK 增加 (target - cwnd) / cwnd
            uint32_t delta = (target - _cwnd) * _mss / _cwnd;
            _cwnd += std::max(delta, 1u);
        }

        // 上限
        _cwnd = std::min(_cwnd, 65535u * 16);
    }

    void on_loss(uint32_t byte_lost) override {
        (void)byte_lost;

        // 记录丢包时的窗口
        if (_cwnd < _w_max) {
            // 快速收敛：如果窗口比上次丢包时小，降低 W_max
            _w_max = _cwnd * (1 + BETA) / 2;
        } else {
            _w_max = _cwnd;
        }

        // 乘法减少
        _ssthresh = static_cast<uint32_t>(_cwnd * BETA);
        _ssthresh = std::max(_ssthresh, MIN_CWND * _mss);
        _cwnd = _ssthresh;

        // 计算 K：到达W_max所需时间
        _k = std::cbrt(_w_max * (1 - BETA) / C);

        // 重置 epoch
        _epoch_start = std::chrono::steady_clock::time_point{};
        _in_slow_start = false;
    }

    void on_timeout() {
        // 超时更严重：重置到慢启动
        _ssthresh = static_cast<uint32_t>(_cwnd * BETA);
        _ssthresh = std::max(_ssthresh, MIN_CWND * _mss);
        _cwnd = _mss;  // 重置为 1 MSS
        _in_slow_start = true;
        _epoch_start = std::chrono::steady_clock::time_point{};
    }

    uint32_t cwnd() const override { return _cwnd; }
    uint32_t ssthresh() const override { return _ssthresh; }

    // 额外接口供 Orca 使用
    double w_max() const { return _w_max; }
    bool in_slow_start() const { return _in_slow_start; }

private:
    double cubic_window(double t) const {
        // W(t) = C * (t - k)^3 + W_max
        double dt = t - _k;
        return C * dt * dt * dt + _w_max;
    }

    uint32_t _mss;
    uint32_t _cwnd;
    uint32_t _ssthresh;

    // CUBIC 特有状态
    double _w_max;
    double _k;
    std::chrono::steady_clock::time_point _epoch_start; // 当前增长周期开始时间
    bool _in_slow_start;

    // TCP 友好模式
    bool _tcp_friendliness;
    double _w_tcp;

    // RTT（用于 TCP 友好模式）
    uint32_t _last_rtt_us = 0;
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_CUBIC_HPP