#ifndef NEUSTACK_TRANSPORT_TCP_RENO_HPP
#define NEUSTACK_TRANSPORT_TCP_RENO_HPP

#include "neustack/transport/tcp_tcb.hpp"
#include <algorithm>

namespace neustack {

// TCP Reno 拥塞控制实现
class TCPReno : public ICongestionControl {
public:
    explicit TCPReno(uint32_t mss = 1460)
        : _mss(mss < 536 ? 536 : mss)  // MSS 最小 536 (RFC 879)
        , _cwnd(_mss)                   // 初始 1 MSS
        , _ssthresh(65535)              // 初始很大
        , _in_fast_recovery(false) {}
    
    void on_ack(uint32_t bytes_acked, uint32_t rtt_us) override {
        (void)rtt_us; // Reno算法不使用 RTT

        if (_in_fast_recovery) {
            // 快速恢复中收到新的 ACK，退出快速恢复
            _cwnd = _ssthresh;
            _in_fast_recovery = false;
            return;
        }

        if (_cwnd < _ssthresh) {
            // 慢启动：每个 ACK 增加 1 MSS（指数增长）
            _cwnd += _mss;
        } else {
            // 拥塞避免：每个 RTT 增加大约 1 MSS（线性增长）
            _cwnd += _mss * _mss / _cwnd;
        }

        // cwnd的上限
        _cwnd = std::min(_cwnd, 65535u * 16);
    }

    void on_loss(uint32_t bytes_lost) override {
        (void)bytes_lost;

        // 进入快速恢复
        _ssthresh = std::max(_cwnd / 2, 2 * _mss);
        _cwnd = _ssthresh + 3 * _mss;
        _in_fast_recovery = true;
    }

    // 超时时调用（比 on_loss 更严重）
    void on_timeout() {
        _ssthresh = std::max(_cwnd / 2, 2 * _mss);
        _cwnd = _mss;  // 重置为 1 MSS
        _in_fast_recovery = false;
    }

    // 快速恢复中收到重复 ACK
    void on_dup_ack() {
        if (_in_fast_recovery) {
            // 每个 dup ACK 增加 1 MSS（膨胀窗口）
            _cwnd += _mss;
        }
    }

    uint32_t cwnd() const override { return _cwnd; }
    uint32_t ssthresh() const override { return _ssthresh; }

    bool in_fast_recovery() const { return _in_fast_recovery; }

private:
    uint32_t _mss;
    uint32_t _cwnd;
    uint32_t _ssthresh;
    bool _in_fast_recovery;
    // 注：NewReno 需要 _recover_seq 来处理部分确认，基础 Reno 不需要
};

} // namespace neustack

#endif // NEUSTACK_TRANSPORT_TCP_RENO_HPP