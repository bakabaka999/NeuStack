#include "neustack/ai/security_model.hpp"
#include "neustack/common/log.hpp"
#include <cmath>
#include <algorithm>

namespace neustack {

// ============================================================================
// 构造函数
// ============================================================================

SecurityAnomalyModel::SecurityAnomalyModel(const std::string& model_path, float threshold)
    : _threshold(threshold)
{
    try {
        _inference = std::make_unique<ONNXInference>(model_path);

        // 验证维度：Autoencoder 输入输出应相同
        if (_inference->input_size() != INPUT_DIM) {
            LOG_WARN(AI, "SecurityAnomalyModel: expected input_size=%zu, got %zu",
                     INPUT_DIM, _inference->input_size());
        }
        if (_inference->input_size() != _inference->output_size()) {
            LOG_WARN(AI, "SecurityAnomalyModel: input/output size mismatch: %zu vs %zu",
                     _inference->input_size(), _inference->output_size());
        }

        // 尝试从模型 metadata 读取阈值
        if (_threshold <= 0.0f) {
            auto meta = _inference->get_metadata("anomaly_threshold");
            if (meta) {
                try {
                    _threshold = std::stof(*meta);
                    LOG_INFO(AI, "SecurityAnomalyModel loaded, threshold=%.6f (from metadata)",
                             _threshold);
                } catch (...) {
                    _threshold = 0.5f; // fallback
                    LOG_WARN(AI, "Invalid threshold in metadata: %s, using default 0.5",
                             meta->c_str());
                }
            } else {
                _threshold = 0.5f;
                LOG_INFO(AI, "SecurityAnomalyModel loaded, threshold=0.5 (default)");
            }
        } else {
            LOG_INFO(AI, "SecurityAnomalyModel loaded, threshold=%.6f (from parameter)",
                     _threshold);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Failed to load security model: %s", e.what());
        _inference = nullptr;
    }
}

// ============================================================================
// 推理
// ============================================================================

std::optional<ISecurityModel::Output> SecurityAnomalyModel::infer(const Input& input) {
    if (!_inference) {
        return std::nullopt;
    }

    auto features = input.to_array();

    try {
        auto reconstructed = _inference->run(features);

        float error = compute_mse(features.data(), reconstructed.data(),
                                  std::min(features.size(), reconstructed.size()));
        bool anomaly = error > _threshold;
        float conf = error_to_confidence(error);

        return Output{
            .reconstruction_error = error,
            .is_anomaly = anomaly,
            .confidence = conf,
        };
    } catch (const std::exception& e) {
        LOG_ERROR(AI, "Security model inference failed: %s", e.what());
        return std::nullopt;
    }
}

// ============================================================================
// 内部方法
// ============================================================================

float SecurityAnomalyModel::compute_mse(const float* input, const float* reconstructed, size_t n) {
    if (n == 0) return 0.0f;
    float mse = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float d = input[i] - reconstructed[i];
        mse += d * d;
    }
    return mse / static_cast<float>(n);
}

float SecurityAnomalyModel::error_to_confidence(float error) const {
    // 基于误差与阈值的距离计算置信度
    // error << threshold → confidence ≈ 0 (确信正常)
    // error == threshold → confidence ≈ 0.5
    // error >> threshold → confidence ≈ 1.0 (确信异常)
    if (_threshold <= 0.0f) return 0.5f;

    // sigmoid: 1 / (1 + exp(-k * (error/threshold - 1)))
    // k 控制斜率，这里用 6.0 使得阈值附近变化较快
    float ratio = error / _threshold;
    float x = 6.0f * (ratio - 1.0f);
    return 1.0f / (1.0f + std::exp(-x));
}

} // namespace neustack
