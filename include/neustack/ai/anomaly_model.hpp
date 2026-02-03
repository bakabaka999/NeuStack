#ifndef NEUSTACK_AI_ANOMALY_MODEL_HPP
#define NEUSTACK_AI_ANOMALY_MODEL_HPP

#include "neustack/ai/onnx_inference.hpp"
#include "neustack/ai/ai_model.hpp"
#include <memory>

namespace neustack {
    
/**
 * LSTM-Autoencoder 异常检测模型
 *
 * 原理: 用正常流量训练自编码器，异常流量的重构误差会显著高于正常流量
 */
class AnomalyDetector : public IAnomalyModel {
public:
    /**
     * 构造异常检测器
     * @param model_path ONNX 模型路径
     * @param threshold 异常阈值（重构误差大于此值判定为异常）
     */
    explicit AnomalyDetector(const std::string &model_path, float threshold = 0.5f);

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char *name() const override { return "AnomalyDetector"; }

    // IAnomalyModel 接口
    std::optional<Output> infer(const Input &input) override;
    void set_threshold(float threshold) override { _threshold = threshold; }

    // 获取当前阈值
    float threshold() const { return _threshold; }

private: 
    std::unique_ptr<ONNXInference> _inference;
    float _threshold;

    // 计算重构误差（MSE）
    float compute_reconstruction_error(
        const std::vector<float> &input,
        const std::vector<float> &reconstructed
    );
};

} // namespace neustack


#endif // NEUSTACK_AI_ANOMALY_MODEL_HPP