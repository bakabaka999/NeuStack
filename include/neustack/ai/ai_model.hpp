#ifndef NEUSTACK_AI_AI_MODEL_HPP
#define NEUSTACK_AI_AI_MODEL_HPP

#include "neustack/ai/onnx_inference.hpp"
#include <optional>
#include <memory>

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
        float syn_rate;
        float rst_rate;
        float new_conn_rate;
        float packet_rate;
        float avg_packet_size;
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

} // namespace neustack

#endif // NEUSTACK_AI_AI_MODEL_HPP