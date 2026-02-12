#ifndef NEUSTACK_AI_AI_MODEL_HPP
#define NEUSTACK_AI_AI_MODEL_HPP

#include <array>
#include <optional>
#include <memory>
#include <vector>

namespace neustack {
    
/**
 * AI 模型基类
 */
class AIModel {
public:
    virtual ~AIModel() = default;

    // 模型是否已加载
    virtual bool is_loaded() const = 0;

    // 获取模型名称
    virtual const char *name() const = 0;
};

/**
 * 拥塞控制模型 (Orca)
 *
 * 输入: OrcaFeatures (7 维)
 * 输出: alpha ∈ [-1, 1], cwnd_new = 2^alpha * cwnd_base
 */
class ICongestionModel : public AIModel {
public:
    struct Input {
        float throughput_normalized;
        float queuing_delay_normalized;
        float rtt_ratio;
        float loss_rate;
        float cwnd_normalized;
        float in_flight_ratio;
        float predicted_bw_normalized;  // 带宽预测结果 (来自 BandwidthPredictor)
    };

    struct Output {
        float alpha; // cwnd 调整因子
    };

    virtual std::optional<Output> infer(const Input &input) = 0;
};

/**
 * 异常检测模型 (LSTM-Autoencoder)
 *
 * 输入: AnomalyFeatures (5 维)
 * 输出: 重构误差，超过阈值则为异常
 */
class IAnomalyModel : public AIModel {
public:
    struct Input {
        float packets_rx_norm;
        float packets_tx_norm;
        float bytes_tx_norm;
        float syn_rate_norm;
        float rst_rate_norm;
        float conn_established_norm;
        float tx_rx_ratio_norm;
        float active_conn_norm;
    };

    struct Output {
        float reconstruction_error;
        bool is_anomaly;
    };

    virtual std::optional<Output> infer(const Input& input) = 0;

    /** 设置异常阈值 */
    virtual void set_threshold(float threshold) = 0;
};

/**
 * 带宽预测模型 (LSTM)
 *
 * 输入: 历史时序数据 (N 个时间步)
 * 输出: 预测带宽 (bytes/s)
 */
class IBandwidthModel : public AIModel {
public:
    struct Input {
        std::vector<float> throughput_history;
        std::vector<float> rtt_history;
        std::vector<float> loss_history;
    };

    struct Output {
        uint32_t predicted_bandwidth;  // bytes/s
        float confidence;              // [0, 1]
    };

    virtual std::optional<Output> infer(const Input& input) = 0;

    /** 获取所需的历史长度 */
    virtual size_t required_history_length() const = 0;
};

/**
 * 安全异常检测模型 (Autoencoder)
 *
 * 专用于防火墙场景，输入为安全特征 (8 维)
 * 与 IAnomalyModel 的区别：特征面向网络安全（SYN ratio、RST ratio 等）
 * 输出增加 confidence 字段
 */
class ISecurityModel : public AIModel {
public:
    static constexpr size_t INPUT_DIM = 8;

    struct Input {
        float pps_norm;             // 包速率
        float bps_norm;             // 字节速率
        float syn_rate_norm;        // SYN 速率
        float rst_rate_norm;        // RST 速率
        float syn_ratio_norm;       // SYN/SYN-ACK 比率
        float new_conn_rate_norm;   // 新连接速率
        float avg_pkt_size_norm;    // 平均包大小
        float rst_ratio_norm;       // RST/总包 比率

        /** 转换为 float 数组 */
        std::array<float, INPUT_DIM> to_array() const {
            return {pps_norm, bps_norm, syn_rate_norm, rst_rate_norm,
                    syn_ratio_norm, new_conn_rate_norm, avg_pkt_size_norm, rst_ratio_norm};
        }
    };

    struct Output {
        float reconstruction_error; // 重构误差 (MSE)
        bool is_anomaly;            // 是否异常
        float confidence;           // 置信度 [0, 1]
    };

    virtual std::optional<Output> infer(const Input& input) = 0;
    virtual void set_threshold(float threshold) = 0;
    virtual float get_threshold() const = 0;
};

} // namespace neustack

#endif // NEUSTACK_AI_AI_MODEL_HPP