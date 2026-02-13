#ifndef NEUSTACK_AI_SECURITY_MODEL_HPP
#define NEUSTACK_AI_SECURITY_MODEL_HPP

#include "neustack/ai/ai_model.hpp"
#include "neustack/ai/onnx_inference.hpp"
#include <memory>
#include <string>

namespace neustack {

/**
 * 安全异常检测模型 - Autoencoder
 *
 * 专用于防火墙流量异常检测，与通用 AnomalyDetector 的区别：
 * - 输入特征面向网络安全 (SYN ratio, RST ratio, avg_pkt_size 等)
 * - 输出增加 confidence（基于误差与阈值的距离）
 * - 支持从模型 metadata 读取阈值和归一化参数
 *
 * 模型结构: Autoencoder (8 → encoder → latent → decoder → 8)
 * 异常判定: reconstruction_error > threshold
 */
class SecurityAnomalyModel : public ISecurityModel {
public:
    /**
     * 构造安全异常检测模型
     * @param model_path ONNX 模型路径
     * @param threshold 异常阈值（MSE），0 表示从模型 metadata 读取
     */
    explicit SecurityAnomalyModel(const std::string& model_path, float threshold = 0.0f);

    // AIModel 接口
    bool is_loaded() const override { return _inference != nullptr; }
    const char* name() const override { return "SecurityAnomalyModel"; }

    // ISecurityModel 接口
    std::optional<Output> infer(const Input& input) override;
    void set_threshold(float threshold) override { _threshold = threshold; }
    float get_threshold() const override { return _threshold; }

private:
    std::unique_ptr<ONNXInference> _inference;
    float _threshold;

    /// 计算 MSE 重构误差
    static float compute_mse(const float* input, const float* reconstructed, size_t n);

    /// 误差 → 置信度映射：离阈值越远置信度越高
    float error_to_confidence(float error) const;
};

} // namespace neustack

#endif // NEUSTACK_AI_SECURITY_MODEL_HPP
