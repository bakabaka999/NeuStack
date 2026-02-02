#ifndef NEUSTACK_METRICS_TCP_SAMPLE_HPP
#define NEUSTACK_METRICS_TCP_SAMPLE_HPP

#include <cstdint>

namespace neustack {

/**
 * TCP 连接采样数据
 *
 * 采集频率: Per-ACK 或每 10ms
 * 消费者: Orca 拥塞控制、带宽预测
 *
 * 内存布局: 48 字节，8 字节对齐
 */
struct TCPSample {
    // ─── 时间戳 (8 bytes) ───
    uint64_t timestamp_us;        // 采样时间戳

    // ─── RTT 相关 (12 bytes) ───
    uint32_t rtt_us;              // 本次 RTT 样本
    uint32_t min_rtt_us;          // 历史最小 RTT (基线延迟)
    uint32_t srtt_us;             // 平滑 RTT

    // ─── 拥塞窗口 (12 bytes) ───
    uint32_t cwnd;                // 当前 cwnd (MSS 单位)
    uint32_t ssthresh;            // 慢启动阈值
    uint32_t bytes_in_flight;     // 在途字节数

    // ─── 吞吐量 (8 bytes) ───
    uint32_t delivery_rate;       // 交付速率 (bytes/s)
    uint32_t send_rate;           // 发送速率 (bytes/s)

    // ─── 拥塞信号 (8 bytes, 含 padding) ───
    uint8_t loss_detected;        // 本周期是否检测到丢包
    uint8_t timeout_occurred;     // 本周期是否超时
    uint8_t ecn_ce_count;         // ECN: 本周期收到的 CE 标记包数量
    uint8_t is_app_limited;       // 应用受限: 发送缓冲区空，delivery_rate 不代表真实带宽
    uint16_t packets_sent;        // 本周期发送的包数
    uint16_t packets_lost;        // 本周期丢失的包数 (重传次数)

    // ─── 派生指标 ───

    // 丢包率 [0, 1]
    float loss_rate() const {
        if (packets_sent == 0) return 0.0f;
        return static_cast<float>(packets_lost) / packets_sent;
    }

    // 排队延迟 (Orca 核心输入)
    uint32_t queuing_delay_us() const {
        if (min_rtt_us == 0 || rtt_us < min_rtt_us) return 0;
        return rtt_us - min_rtt_us;
    }

    // RTT 比值
    float rtt_ratio() const {
        if (min_rtt_us == 0) return 1.0f;
        return static_cast<float>(rtt_us) / min_rtt_us;
    }

    // 是否可用于带宽估计 (非 app limited 且无超时)
    bool is_valid_for_bw_estimation() const {
        return !is_app_limited && !timeout_occurred;
    }
};

// 编译期检查大小
static_assert(sizeof(TCPSample) == 48, "TCPSample should be 48 bytes");

} // namespace neustack

#endif // NEUSTACK_METRICS_TCP_SAMPLE_HPP
